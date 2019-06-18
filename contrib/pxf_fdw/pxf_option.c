/*
 * pxf_option.c
 *		  Foreign-data wrapper option handling for PXF (Platform Extension Framework)
 *
 * IDENTIFICATION
 *		  contrib/pxf_fdw/pxf_option.c
 */

#include "postgres.h"

#include "pxf_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"

static char *const FDW_OPTION_FORMAT_TEXT = "text";
static char *const FDW_OPTION_FORMAT_CSV = "csv";
static char *const FDW_OPTION_FORMAT_RC = "rc";

static char *const FDW_OPTION_REJECT_LIMIT_ROWS = "rows";
static char *const FDW_OPTION_REJECT_LIMIT_PERCENT = "percent";

static char *const FDW_OPTION_PROTOCOL = "protocol";
static char *const FDW_OPTION_RESOURCE = "resource";
static char *const FDW_OPTION_FORMAT = "format";
static char *const FDW_OPTION_REJECT_LIMIT_TYPE = "reject_limit_type";	/* valid types are row
																		 * and percent */
static char *const FDW_OPTION_REJECT_LIMIT = "reject_limit";
static char *const FDW_OPTION_WIRE_FORMAT = "wire_format";
static char *const FDW_OPTION_PXF_PORT = "pxf_port";
static char *const FDW_OPTION_PXF_HOST = "pxf_host";
static char *const FDW_OPTION_PXF_PROTOCOL = "pxf_protocol";

/*
 * Describes the valid copy options for objects that use this wrapper.
 */
struct PxfFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

static const struct PxfFdwOption valid_options[] = {
	{FDW_OPTION_PROTOCOL, ForeignDataWrapperRelationId},
	{FDW_OPTION_RESOURCE, ForeignTableRelationId},
	{FDW_OPTION_FORMAT, ForeignTableRelationId},
	{FDW_OPTION_WIRE_FORMAT, ForeignTableRelationId},

	/* Error handling */
	{FDW_OPTION_REJECT_LIMIT, ForeignTableRelationId},
	{FDW_OPTION_REJECT_LIMIT_TYPE, ForeignTableRelationId},

	/* Sentinel */
	{NULL, InvalidOid}
};

/*
 * Valid COPY options for *_pxf_fdw.
 * These options are based on the options for the COPY FROM command.
 * But note that force_not_null and force_null are handled as boolean options
 * attached to a column, not as table options.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 */
static const struct PxfFdwOption valid_copy_options[] = {
	/* Format options */
	/* oids option is not supported */
	/* freeze option is not supported */
	{"format", ForeignTableRelationId},
	{"header", ForeignTableRelationId},
	{"delimiter", ForeignTableRelationId},
	{"quote", ForeignTableRelationId},
	{"escape", ForeignTableRelationId},
	{"null", ForeignTableRelationId},
	{"encoding", ForeignTableRelationId},
	{"newline", ForeignTableRelationId},
	{"fill_missing_fields", ForeignTableRelationId},
	{"force_not_null", AttributeRelationId},
	{"force_null", AttributeRelationId},

	/*
	 * {"force_quote", ForeignTableRelationId}, force_quote is not supported
	 * by file_fdw because it's for COPY TO.
	 */

	/* Sentinel */
	{NULL, InvalidOid}
};

extern Datum pxf_fdw_validator(PG_FUNCTION_ARGS);

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(pxf_fdw_validator);

/*
 * Helper functions
 */
static Datum ValidateCopyOptions(List *options_list, Oid catalog);

static bool IsCopyOption(const char *option);

static bool IsValidCopyOption(const char *option, Oid context);

static const char *GetWireFormatName(const char *format);

static void ValidateOption(char *, Oid);

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 *
 */
Datum
pxf_fdw_validator(PG_FUNCTION_ARGS)
{
	char	   *protocol = NULL;
	char	   *resource = NULL;
	char	   *reject_limit_type = FDW_OPTION_REJECT_LIMIT_ROWS;
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	List	   *copy_options = NIL;
	ListCell   *cell;
	int			reject_limit = -1;

	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

/*		check whether option is valid at it's catalog level, if not, valid error out */
		ValidateOption(def->defname, catalog);

		if (strcmp(def->defname, FDW_OPTION_PROTOCOL) == 0)
			protocol = defGetString(def);
		else if (strcmp(def->defname, FDW_OPTION_RESOURCE) == 0)
			resource = defGetString(def);
		else if (strcmp(def->defname, FDW_OPTION_WIRE_FORMAT) == 0)
		{
			char	   *value = defGetString(def);

			if (strcmp(TextFormatName, value) != 0 && strcmp(GpdbWritableFormatName, value) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg(
								"invalid wire_format value, only '%s' and '%s' are supported", TextFormatName,
								GpdbWritableFormatName)));
		}
		else if (strcmp(def->defname, FDW_OPTION_FORMAT) == 0)
		{
			/*
			 * Format option in PXF is different from the COPY format option.
			 * In PXF, format refers to the file format on the external
			 * system, for example Parquet, Avro, Text, CSV.
			 *
			 * For COPY, the format can only be text, csv, or binary. pxf_fdw
			 * leverages the csv format in COPY.
			 */
			char	   *value = defGetString(def);

			if (pg_strcasecmp(value, FDW_OPTION_FORMAT_TEXT) == 0 || pg_strcasecmp(value, FDW_OPTION_FORMAT_CSV) == 0)
				copy_options = lappend(copy_options, def);
		}
		else if (strcmp(def->defname, FDW_OPTION_REJECT_LIMIT) == 0)
		{
			char	   *endptr = NULL;
			char	   *pStr = defGetString(def);

			reject_limit = (int) strtol(pStr, &endptr, 10);

			if (pStr == endptr || reject_limit < 1)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
						 errmsg(
								"invalid reject_limit value '%s', should be a positive integer", pStr)));
		}
		else if (strcmp(def->defname, FDW_OPTION_REJECT_LIMIT_TYPE) == 0)
		{
			reject_limit_type = defGetString(def);
			if (pg_strcasecmp(reject_limit_type, FDW_OPTION_REJECT_LIMIT_ROWS) != 0 &&
				pg_strcasecmp(reject_limit_type, FDW_OPTION_REJECT_LIMIT_PERCENT) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
						 errmsg(
								"invalid reject_limit_type value, only '%s' and '%s' are supported",
								FDW_OPTION_REJECT_LIMIT_ROWS, FDW_OPTION_REJECT_LIMIT_PERCENT)));
		}
		else if (IsCopyOption(def->defname))
			copy_options = lappend(copy_options, def);
	}

	if (catalog == ForeignDataWrapperRelationId &&
		(protocol == NULL || strcmp(protocol, "") == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg(
						"the protocol option must be defined for PXF foreign-data wrappers")));
	}

	if (catalog == ForeignTableRelationId &&
		(resource == NULL || strcmp(resource, "") == 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg(
						"the resource option must be defined at the foreign table level")));
	}

	/* Validate reject limit */
	if (reject_limit != -1)
	{
		if (pg_strcasecmp(reject_limit_type, FDW_OPTION_REJECT_LIMIT_ROWS) == 0)
		{
			if (reject_limit < 2)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
						 errmsg(
								"invalid (ROWS) reject_limit value '%d', valid values are 2 or larger",
								reject_limit)));
		}
		else
		{
			if (reject_limit < 1 || reject_limit > 100)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
						 errmsg(
								"invalid (PERCENT) reject_limit value '%d', valid values are 1 to 100",
								reject_limit)));
		}
	}

	/*
	 * Additional validations for Copy options
	 */
	ValidateCopyOptions(copy_options, catalog);

	PG_RETURN_VOID();
}

Datum
ValidateCopyOptions(List *options_list, Oid catalog)
{
	DefElem    *force_not_null = NULL;
	DefElem    *force_null = NULL;
	List	   *copy_options = NIL;
	ListCell   *cell;

	/*
	 * Check that only options supported by copy, and allowed for the current
	 * object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!IsValidCopyOption(def->defname, catalog))
		{
			const struct PxfFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_copy_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);
			}

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 buf.len > 0
					 ? errhint("Valid options in this context are: %s",
							   buf.data)
					 : errhint("There are no valid options in this context.")));
		}

		/*
		 * force_not_null is a boolean option; after validation we can discard
		 * it - it will be retrieved later in get_file_fdw_attribute_options()
		 */
		if (strcmp(def->defname, "force_not_null") == 0)
		{
			if (force_not_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("option \"force_not_null\" supplied more than once for a column")));
			force_not_null = def;
			/* Don't care what the value is, as long as it's a legal boolean */
			(void) defGetBoolean(def);
		}
		/* See comments for force_not_null above */
		else if (strcmp(def->defname, "force_null") == 0)
		{
			if (force_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("option \"force_null\" supplied more than once for a column")));
			force_null = def;
			(void) defGetBoolean(def);
		}
		else
			copy_options = lappend(copy_options, def);
	}

	/*
	 * Apply the core COPY code's validation logic for more checks.
	 */
	ProcessCopyOptions(NULL, true, copy_options, 0, true);

	PG_RETURN_VOID();
}

/*
 * Fetch the options for a pxf_fdw foreign table.
 */
PxfOptions *
PxfGetOptions(Oid foreigntableid)
{
	UserMapping *user;
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;
	List	   *options;
	PxfOptions *opt;
	ListCell   *lc;
	List	   *copy_options,
			   *other_options,
			   *other_option_names = NULL;

	opt = (PxfOptions *) palloc(sizeof(PxfOptions));
	memset(opt, 0, sizeof(PxfOptions));

	copy_options = NIL;
	other_options = NIL;

	opt->reject_limit = -1;
	opt->is_reject_limit_rows = true;

	/*
	 * Extract options from FDW objects.
	 */
	table = GetForeignTable(foreigntableid);
	user = GetUserMapping(GetUserId(), server->serverid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, table->options);
	options = list_concat(options, user->options);
	options = list_concat(options, server->options);
	options = list_concat(options, wrapper->options);

	/* Loop through the options, and get the server/port */
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, FDW_OPTION_PXF_HOST) == 0)
			opt->pxf_host = defGetString(def);

		else if (strcmp(def->defname, FDW_OPTION_PXF_PORT) == 0)
		{
			opt->pxf_port = atoi(defGetString(def));
			/* TODO: maybe consider validating in pxf_fdw_validator */
			if (opt->pxf_port <= 0 || opt->pxf_port >= 65535)
				elog(ERROR, "invalid port number: %s", defGetString(def));
		}
		else if (strcmp(def->defname, FDW_OPTION_PXF_PROTOCOL) == 0)
			opt->pxf_protocol = defGetString(def);
		else if (strcmp(def->defname, FDW_OPTION_PROTOCOL) == 0)
			opt->protocol = defGetString(def);
		else if (strcmp(def->defname, FDW_OPTION_RESOURCE) == 0)
			opt->resource = defGetString(def);
		else if (strcmp(def->defname, FDW_OPTION_REJECT_LIMIT) == 0)
			opt->reject_limit = atoi(defGetString(def));
		else if (strcmp(def->defname, FDW_OPTION_REJECT_LIMIT_TYPE) == 0)
			opt->is_reject_limit_rows = pg_strcasecmp(FDW_OPTION_REJECT_LIMIT_ROWS, defGetString(def)) == 0;
		else if (strcmp(def->defname, FDW_OPTION_FORMAT) == 0)
		{
			opt->format = defGetString(def);

			if (pg_strcasecmp(opt->format, FDW_OPTION_FORMAT_TEXT) == 0 ||
				pg_strcasecmp(opt->format, FDW_OPTION_FORMAT_CSV) == 0)
				copy_options = lappend(copy_options, def);
		}
		else if (strcmp(def->defname, FDW_OPTION_WIRE_FORMAT) == 0)
			opt->wire_format = defGetString(def);
		else if (IsCopyOption(def->defname))
			copy_options = lappend(copy_options, def);
		else
		{
			Value	   *val = makeString(def->defname);

			if (!list_member(other_option_names, val))
			{
				other_options = lappend(other_options, def);
				other_option_names = lappend(other_option_names, val);
			}
		}
	} //foreach

		opt->copy_options = copy_options;
	opt->options = other_options;

	opt->server = server->servername;
	opt->exec_location = wrapper->exec_location;

	/*
	 * The profile corresponds to protocol[:format]
	 */
	opt->profile = opt->protocol;

	if (opt->format)
		opt->profile = psprintf("%s:%s", opt->protocol, opt->format);

	/*
	 * Set defaults when not provided
	 */
	if (!opt->pxf_host)
		opt->pxf_host = PXF_FDW_DEFAULT_HOST;

	if (!opt->pxf_port)
		opt->pxf_port = PXF_FDW_DEFAULT_PORT;

	if (!opt->pxf_protocol)
		opt->pxf_protocol = PXF_FDW_DEFAULT_PROTOCOL;

	if (!opt->wire_format)
		opt->wire_format = GetWireFormatName(opt->format);

	return opt;
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
IsValidCopyOption(const char *option, Oid context)
{
	const struct PxfFdwOption *entry;

	for (entry = valid_copy_options; entry->optname; entry++)
	{
		if (context == entry->optcontext && strcmp(entry->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Check if the option is a COPY option
 */
static bool
IsCopyOption(const char *option)
{
	const struct PxfFdwOption *entry;

	for (entry = valid_copy_options; entry->optname; entry++)
	{
		if (strcmp(entry->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Converts a character code for the format name into a string of format definition
 */
static const char *
GetWireFormatName(const char *format)
{
	/* for text we can also have text:multi so we search for "text" */
	if (format && (strcasestr(format, FDW_OPTION_FORMAT_TEXT) || pg_strcasecmp(format, FDW_OPTION_FORMAT_CSV) ||
				   pg_strcasecmp(format, FDW_OPTION_FORMAT_RC)))
		return TextFormatName;
	return GpdbWritableFormatName;
}

/*
 * Goes through standard list of options to make sure option is defined at the correct catalog level
 */
static void
ValidateOption(char *option, Oid catalog)
{
	const struct PxfFdwOption *entry;

	for (entry = valid_options; entry->optname; entry++)
	{
		/* option can only be defined at its catalog level */
		if (strcmp(entry->optname, option) == 0 && catalog != entry->optcontext)
		{
			Relation	rel = RelationIdGetRelation(entry->optcontext);

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg(
							"the %s option can only be defined at the %s level",
							option,
							RelationGetRelationName(rel))));
		}
	}
}
