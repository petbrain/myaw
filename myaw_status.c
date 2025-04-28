#include <myaw.h>

PwTypeId PwTypeId_MwStatus = 0;

uint16_t MW_END_OF_BLOCK = 0;
uint16_t MW_PARSE_ERROR = 0;

PwResult _mw_parser_error(MwParser* parser, char* source_file_name, unsigned source_line_number,
                           unsigned line_number, unsigned char_pos, char* description, ...)
{
    PwValue status = pw_create(PwTypeId_MwStatus);
    // status is PW_SUCCESS by default
    // can't use pw_if_error here because of simplified checking in pw_ok
    if (status.status_code != PW_SUCCESS) {
        return pw_move(&status);
    }

    // the base Status constructor does not allocate struct_data for PW_SUCCESS
    // do this by setting status code and description

    status.status_code = MW_PARSE_ERROR;
    _pw_set_status_desc(&status, "");
    if (status.struct_data == nullptr) {
        return PwOOM();
    }

    _pw_set_status_location(&status, source_file_name, source_line_number);
    MwStatusData* status_data = _mw_status_data_ptr(&status);
    status_data->line_number = line_number;;
    status_data->position = char_pos;

    va_list ap;
    va_start(ap);
    _pw_set_status_desc_ap(&status, description, ap);
    va_end(ap);
    return pw_move(&status);
}

static PwResult mw_status_init(PwValuePtr self, void* ctor_args)
{
    MwStatusData* data = _mw_status_data_ptr(self);
    data->line_number = 0;
    data->position = 0;
    return PwOK();
}

static void mw_status_hash(PwValuePtr self, PwHashContext* ctx)
{
    MwStatusData* data = _mw_status_data_ptr(self);

    _pw_hash_uint64(ctx, self->type_id);
    _pw_hash_uint64(ctx, data->line_number);
    _pw_hash_uint64(ctx, data->position);

    // call super method

    pw_ancestor_of(PwTypeId_MwStatus)->hash(self, ctx);
}

static PwResult mw_status_to_string(PwValuePtr self)
{
    MwStatusData* data = _mw_status_data_ptr(self);

    char location[48];
    snprintf(location, sizeof(location), "Line %u, position %u: ",
             data->line_number, data->position);

    PwValue result = pw_create_string(location);
    pw_return_if_error(&result);

    PwValue status_str = pw_ancestor_of(PwTypeId_MwStatus)->to_string(self);
    pw_return_if_error(&status_str);

    if (!pw_string_append(&result, &status_str)) {
        return PwOOM();
    }

    return pw_move(&result);
}

static PwType mw_status_type;

[[ gnu::constructor ]]
static void init_mw_status()
{
    PwTypeId_MwStatus = pw_struct_subtype(&mw_status_type, "MwStatus", PwTypeId_Status, MwStatusData);
    mw_status_type.init      = mw_status_init;
    mw_status_type.hash      = mw_status_hash;
    mw_status_type.to_string = mw_status_to_string;

    // init status codes
    MW_END_OF_BLOCK = pw_define_status("END_OF_BLOCK");
    MW_PARSE_ERROR  = pw_define_status("PARSE_ERROR");
}
