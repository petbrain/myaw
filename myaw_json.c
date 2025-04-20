#include <myaw.h>
#include <pw_parse.h>

static char32_t number_terminators[] = { MW_COMMENT, ':', ',', '}', ']', 0 };


static PwResult skip_spaces(MwParser* parser, unsigned* pos, unsigned source_line)
/*
 * Skip spaces and comments before structural element.
 *
 * On success return Unsigned value containing first non-space character.
 *
 * On error `source_line` is set in returned status.
 */
{
    for (;;) {
        PwValuePtr current_line = &parser->current_line;

        *pos = pw_string_skip_spaces(current_line, *pos);

        // end of line?
        if (pw_string_index_valid(current_line, *pos)) {
            // no, return character if not a comment
            char32_t chr = pw_char_at(current_line, *pos);
            if (chr != '#') {
                return PwUnsigned(chr);
            }
        }
        // read next line
        PwValue status = _mw_read_block_line(parser);
        if (_mw_end_of_block(&status)) {
            PwValue error = mw_parser_error(parser, parser->current_indent, "Unexpected end of block");
            if (error.status_code == MW_PARSE_ERROR) {
                _pw_set_status_location(&error, __FILE__, source_line);
            }
            return pw_move(&error);
        }
        *pos = parser->current_indent;
    }
}

static PwResult parse_number(MwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the sign or first digit
 */
{
    int sign = 1;
    char32_t chr = pw_char_at(&parser->current_line, start_pos);
    if (chr == '+') {
        // no op
        start_pos++;
    } else if (chr == '-') {
        sign = -1;
        start_pos++;
    }
    return _pw_parse_number(&parser->current_line, start_pos, sign, end_pos, number_terminators);
}

static PwResult parse_string(MwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the opening double quotation mark (")
 */
{
    unsigned closing_quote_pos;
    if (_mw_find_closing_quote(&parser->current_line, '"', start_pos + 1, &closing_quote_pos)) {
        *end_pos = closing_quote_pos + 1;
        return _mw_unescape_line(parser, &parser->current_line,
                                  parser->line_number, '"', start_pos + 1, closing_quote_pos);
    }
    return mw_parser_error(parser, parser->current_indent, "String has no closing quote");
}

static PwResult parse_array(MwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the next character after opening square bracket
 */
{
    parser->json_depth++;

    PwValue result = PwArray();
    pw_return_if_error(&result);

    PwValue chr = skip_spaces(parser, &start_pos, __LINE__);
    pw_return_if_error(&chr);

    if (chr.unsigned_value == ']') {
        // empty array
        *end_pos = start_pos + 1;
        parser->json_depth--;
        return pw_move(&result);
    }

    // parse first item
    PwValue first_item = _mw_parse_json_value(parser, start_pos, &start_pos);
    pw_return_if_error(&first_item);

    pw_expect_ok( pw_array_append(&result, &first_item) );

    // parse subsequent items
    for (;;) {{
        chr = skip_spaces(parser, &start_pos, __LINE__);
        pw_return_if_error(&chr);

        if (chr.unsigned_value == ']') {
            // done
            *end_pos = start_pos + 1;
            parser->json_depth--;
            return pw_move(&result);
        }
        if (chr.unsigned_value != ',') {
            return mw_parser_error(parser, parser->current_indent, "Array items must be separated with comma");
        }
        PwValue item = _mw_parse_json_value(parser, start_pos + 1, &start_pos);
        pw_return_if_error(&item);

        pw_expect_ok( pw_array_append(&result, &item) );
    }}
}

static PwResult parse_object_member(MwParser* parser, unsigned* pos, PwValuePtr result)
/*
 * Parse key:value pair starting from `pos` and update `result`.
 *
 * Update `pos` on exit.
 */
{
    PwValue key = parse_string(parser, *pos, pos);
    pw_return_if_error(&key);

    PwValue chr = skip_spaces(parser, pos, __LINE__);
    pw_return_if_error(&chr);

    if (chr.unsigned_value != ':') {
        return mw_parser_error(parser, *pos, "Values must be separated from keys with colon");
    }

    (*pos)++;

    PwValue value = _mw_parse_json_value(parser, *pos, pos);
    pw_return_if_error(&value);

    return pw_map_update(result, &key, &value);
}

static PwResult parse_object(MwParser* parser, unsigned start_pos, unsigned* end_pos)
/*
 * `start_pos` points to the next character after opening curly bracket
 */
{
    parser->json_depth++;

    PwValue result = PwMap();
    pw_return_if_error(&result);

    PwValue chr = skip_spaces(parser, &start_pos, __LINE__);
    pw_return_if_error(&chr);

    if (chr.unsigned_value == '}') {
        // empty object
        *end_pos = start_pos + 1;
        parser->json_depth--;
        return pw_move(&result);
    }

    // parse first member
    PwValue status = parse_object_member(parser, &start_pos, &result);
    pw_return_if_error(&status);

    // parse subsequent members
    for (;;) {{
        chr = skip_spaces(parser, &start_pos, __LINE__);
        pw_return_if_error(&chr);

        if (chr.unsigned_value == '}') {
            // done
            *end_pos = start_pos + 1;
            parser->json_depth--;
            return pw_move(&result);
        }
        if (chr.unsigned_value != ',') {
            return mw_parser_error(parser, parser->current_indent, "Object members must be separated with comma");
        }
        start_pos++;
        chr = skip_spaces(parser, &start_pos, __LINE__);
        pw_return_if_error(&chr);

        PwValue status = parse_object_member(parser, &start_pos, &result);
        pw_return_if_error(&status);
    }}
}

PwResult _mw_parse_json_value(MwParser* parser, unsigned start_pos, unsigned* end_pos)
{
    if (parser->json_depth >= parser->max_json_depth) {
        return mw_parser_error(parser, parser->current_indent, "Maximum recursion depth exceeded");
    }

    PwValue first_char = skip_spaces(parser, &start_pos, __LINE__);
    pw_return_if_error(&first_char);

    char32_t chr = first_char.unsigned_value;

    if (chr == '[') {
        return parse_array(parser, start_pos + 1, end_pos);
    }
    if (chr == '{') {
        return parse_object(parser, start_pos + 1, end_pos);
    }
    if (chr == '"') {
        return parse_string(parser, start_pos, end_pos);
    }
    if (chr == '+' || chr == '-' || pw_isdigit(chr)) {
        return parse_number(parser, start_pos, end_pos);
    }
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "null")) {
        *end_pos = start_pos + 4;
        return PwNull();
    }
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "true")) {
        *end_pos = start_pos + 4;
        return PwBool(true);
    }
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 5, "false")) {
        *end_pos = start_pos + 5;
        return PwBool(false);
    }
    return mw_parser_error(parser, start_pos, "Unexpected character");
}

PwResult _mw_json_parser_func(MwParser* parser)
{
    unsigned end_pos;
    PwValue result = _mw_parse_json_value(parser, _mw_get_start_position(parser), &end_pos);
    pw_return_if_error(&result);

    // check trailing characters

    static char garbage[] = "Gabage after JSON value";

    if (_mw_comment_or_end_of_line(parser, end_pos)) {

        // make sure current block has no more data
        PwValue status = _mw_read_block_line(parser);
        if (!_mw_end_of_block(&status)) {
            return mw_parser_error(parser, parser->current_indent, garbage);
        }
    } else {
        return mw_parser_error(parser, parser->current_indent, garbage);
    }
    return pw_move(&result);
}

PwResult mw_parse_json(PwValuePtr markup)
{
    [[ gnu::cleanup(mw_delete_parser) ]] MwParser* parser = mw_create_parser(markup);
    if (!parser) {
        return PwOOM();
    }
    // read first line to prepare for parsing and to detect EOF
    PwValue status = _mw_read_block_line(parser);
    pw_return_if_error(&status);

    // parse root value
    unsigned end_pos;
    PwValue result = _mw_parse_json_value(parser, 0, &end_pos);
    pw_return_if_error(&result);

    // make sure markup has no more data

    static char extra_data[] = "Extra data after parsed value";

    if (!_mw_comment_or_end_of_line(parser, end_pos)) {
        return mw_parser_error(parser, parser->current_indent, extra_data);
    }
    // make sure current block has no more data
    status = _mw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        pw_return_if_error(&status);
        return mw_parser_error(parser, parser->current_indent, extra_data);
    }
    return pw_move(&result);
}
