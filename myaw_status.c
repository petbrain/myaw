#include <myaw.h>

PwTypeId PwTypeId_MwStatus = 0;

uint16_t MW_END_OF_BLOCK = 0;
uint16_t MW_PARSE_ERROR = 0;

static PwResult mw_status_create(PwTypeId type_id, void* ctor_args)
{
    // call super method

    PwValue status = pw_ancestor_of(PwTypeId_MwStatus)->create(type_id, ctor_args);
    // the super method returns PW_SUCCESS by default
    pw_return_if_error(&status);

    // the base Status constructor may not allocate struct_data
    // do this by setting status description

    _pw_set_status_desc(&status, "");
    if (status.struct_data == nullptr) {
        return PwOOM();
    }
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
    mw_status_type.create    = mw_status_create;
    mw_status_type.init      = mw_status_init;
    mw_status_type.hash      = mw_status_hash;
    mw_status_type.to_string = mw_status_to_string;

    // init status codes
    MW_END_OF_BLOCK = pw_define_status("END_OF_BLOCK");
    MW_PARSE_ERROR  = pw_define_status("PARSE_ERROR");
}
