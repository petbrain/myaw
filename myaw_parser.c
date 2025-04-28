#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <myaw.h>
#include <pw_parse.h>

#define DEFAULT_LINE_CAPACITY  250

#ifdef TRACE_ENABLED
    static unsigned tracelevel = 0;

#   define _TRACE_INDENT() \
        for (unsigned i = 0; i < tracelevel * 4; i++) {  \
            fputc(' ', stderr);  \
        }

#   define _TRACE_POS()  \
        _TRACE_INDENT() \
        fprintf(stderr, "%s; line %u, block indent %u", \
                __func__, parser->line_number, parser->block_indent);

#   define TRACE_ENTER() \
        do {  \
            _TRACE_POS() \
            fputs(" {\n", stderr);  \
            tracelevel++; \
        } while (false)

#   define TRACE_EXIT() \
        do {  \
            tracelevel--; \
            _TRACE_INDENT() \
            fputs("}\n", stderr);  \
        } while (false)

#   define TRACEPOINT()  \
        do {  \
            _TRACE_POS() \
            fputc('\n', stderr);  \
        } while (false)

#   define TRACE(...)  \
        do {  \
            _TRACE_INDENT() \
            fprintf(stderr, "%s: ", __func__); \
            fprintf(stderr, __VA_ARGS__);  \
            fputc('\n', stderr);  \
        } while (false)
#else
#   define TRACEPOINT()
#   define TRACE_ENTER()
#   define TRACE_EXIT()
#   define TRACE(...)
#endif

// forward declarations
static PwResult parse_value(MwParser* parser, unsigned* nested_value_pos, PwValuePtr convspec);
static PwResult value_parser_func(MwParser* parser);
static PwResult parse_raw_value(MwParser* parser);
static PwResult parse_literal_string(MwParser* parser);
static PwResult parse_folded_string(MwParser* parser);
static PwResult parse_datetime(MwParser* parser);
static PwResult parse_timestamp(MwParser* parser);

static char32_t number_terminators[] = { MW_COMMENT, ':', 0 };


MwParser* mw_create_parser(PwValuePtr markup)
{
    MwParser* parser = allocate(sizeof(MwParser), true);
    if (!parser) {
        return nullptr;
    }
    parser->markup = pw_clone(markup);

    parser->blocklevel = 1;
    parser->max_blocklevel = MW_MAX_RECURSION_DEPTH;

    parser->json_depth = 1;
    parser->max_json_depth = MW_MAX_RECURSION_DEPTH;

    parser->skip_comments = true;

    PwValue status = PwNull();

    parser->current_line = pw_create_empty_string(DEFAULT_LINE_CAPACITY, 1);
    if (pw_error(&parser->current_line)) {
        goto error;
    }
    parser->custom_parsers = PwMap(
        PwCharPtr("raw"),       PwPtr((void*) parse_raw_value),
        PwCharPtr("literal"),   PwPtr((void*) parse_literal_string),
        PwCharPtr("folded"),    PwPtr((void*) parse_folded_string),
        PwCharPtr("datetime"),  PwPtr((void*) parse_datetime),
        PwCharPtr("timestamp"), PwPtr((void*) parse_timestamp),
        PwCharPtr("json"),      PwPtr((void*) _mw_json_parser_func)
    );
    if (pw_error(&parser->custom_parsers)) {
        goto error;
    }

    status = pw_start_read_lines(markup);
    if (pw_error(&status)) {
        goto error;
    }

    return parser;

error:
    mw_delete_parser(&parser);
    return nullptr;
}

void mw_delete_parser(MwParser** parser_ptr)
{
    MwParser* parser = *parser_ptr;
    *parser_ptr = nullptr;
    pw_destroy(&parser->markup);
    pw_destroy(&parser->current_line);
    pw_destroy(&parser->custom_parsers);
    release((void**) &parser, sizeof(MwParser));
}

PwResult mw_set_custom_parser(MwParser* parser, char* convspec, MwBlockParserFunc parser_func)
{
    PWDECL_CharPtr(key, convspec);
    PWDECL_Ptr(value, (void*) parser_func);
    return pw_map_update(&parser->custom_parsers, &key, &value);
}

static inline bool have_custom_parser(MwParser* parser, PwValuePtr convspec)
{
    return pw_map_has_key(&parser->custom_parsers, convspec);
}

static inline MwBlockParserFunc get_custom_parser(MwParser* parser, PwValuePtr convspec)
{
    PwValue parser_func = pw_map_get(&parser->custom_parsers, convspec);
    pw_assert_ptr(&parser_func);
    return (MwBlockParserFunc) (parser_func.ptr);
}

bool _mw_end_of_block(PwValuePtr status)
{
    return (status->type_id == PwTypeId_Status) && (status->status_code == MW_END_OF_BLOCK);
}

static inline bool end_of_line(PwValuePtr str, unsigned position)
/*
 * Return true if position is beyond end of line.
 */
{
    return !pw_string_index_valid(str, position);
}

static inline bool isspace_or_eol_at(PwValuePtr str, unsigned position)
{
    if (end_of_line(str, position)) {
        return true;
    } else {
        return pw_isspace(pw_char_at(str, position));
    }
}

static PwResult read_line(MwParser* parser)
/*
 * Read line into parser->current line and strip trailing spaces.
 * Return status.
 */
{
    PwValue status = pw_read_line_inplace(&parser->markup, &parser->current_line);
    pw_return_if_error(&status);

    // strip trailing spaces
    if (!pw_string_rtrim(&parser->current_line)) {
        return PwOOM();
    }

    // measure indent
    parser->current_indent = pw_string_skip_spaces(&parser->current_line, 0);

    // set current_line
    parser->line_number = pw_get_line_number(&parser->markup);

    return PwOK();
}

static inline bool is_comment_line(MwParser* parser)
/*
 * Return true if current line starts with MW_COMMENT char.
 */
{
    return pw_char_at(&parser->current_line, parser->current_indent) == MW_COMMENT;
}

PwResult _mw_read_block_line(MwParser* parser)
{
    TRACEPOINT();

    if (parser->eof) {
        if (parser->blocklevel) {
            // continue returning this for nested blocks
            return PwError(MW_END_OF_BLOCK);
        }
        return PwError(PW_ERROR_EOF);
    }
    for (;;) {{
        PwValue status = read_line(parser);
        if (pw_eof(&status)) {
            parser->eof = true;
            pw_destroy(&parser->current_line);
            return PwError(MW_END_OF_BLOCK);
        }
        pw_return_if_error(&status);

        if (parser->skip_comments) {
            // skip empty lines too
            if (pw_strlen(&parser->current_line) == 0) {
                continue;
            }
            if (is_comment_line(parser)) {
                continue;
            }
            parser->skip_comments = false;
        }
        if (pw_strlen(&parser->current_line) == 0) {
            // return empty line as is
            return PwOK();
        }
        if (parser->current_indent >= parser->block_indent) {
            // indentation is okay, return line
            return PwOK();
        }
        // unindent detected
        if (is_comment_line(parser)) {
            // skip unindented comments
            continue;
        }
        TRACE("unindent");
        // end of block
        if (!pw_unread_line(&parser->markup, &parser->current_line)) {
            return PwError(PW_ERROR_UNREAD_FAILED);
        }
        pw_string_truncate(&parser->current_line, 0);
        return PwError(MW_END_OF_BLOCK);
    }}
}

PwResult _mw_read_block(MwParser* parser)
{
    TRACEPOINT();

    PwValue lines = PwArray();
    pw_return_if_error(&lines);

    for (;;) {{
        // append line
        PwValue line = pw_substr(&parser->current_line, parser->block_indent, UINT_MAX);
        pw_return_if_error(&line);

        pw_expect_ok( pw_array_append(&lines, &line) );

        // read next line
        PwValue status = _mw_read_block_line(parser);
        if (_mw_end_of_block(&status)) {
            return pw_move(&lines);
        }
        pw_return_if_error(&status);
    }}
}

static PwResult parse_nested_block(MwParser* parser, unsigned block_pos, MwBlockParserFunc parser_func)
/*
 * Set block indent to `block_pos` and call parser_func.
 */
{
    if (parser->blocklevel >= parser->max_blocklevel) {
        return mw_parser_error(parser, parser->current_indent, "Too many nested blocks");
    }

    // start nested block
    parser->blocklevel++;
    unsigned saved_block_indent = parser->block_indent;
    parser->block_indent = block_pos;

    TRACE_ENTER();

    // call parser function
    PwValue result = parser_func(parser);

    // end nested block
    parser->block_indent = saved_block_indent;
    parser->blocklevel--;

    TRACE_EXIT();
    return pw_move(&result);
}

static PwResult parse_nested_block_from_next_line(MwParser* parser, MwBlockParserFunc parser_func)
/*
 * Read next line, set block indent to current indent plus one, and call parser_func.
 */
{
    TRACEPOINT();
    TRACE("new block_pos %u", parser->block_indent + 1);

    // temporarily increment block indent by one and read next line
    parser->block_indent++;
    parser->skip_comments = true;
    PwValue status = _mw_read_block_line(parser);
    parser->block_indent--;

    if (_mw_end_of_block(&status)) {
        return mw_parser_error(parser, parser->current_indent, "Empty block");
    }
    pw_return_if_error(&status);

    // call parse_nested_block
    return parse_nested_block(parser, parser->block_indent + 1, parser_func);
}

unsigned _mw_get_start_position(MwParser* parser)
{
    if (parser->block_indent < parser->current_indent) {
        return parser->current_indent;
    } else {
        return pw_string_skip_spaces(&parser->current_line, parser->block_indent);
    }
}

bool _mw_comment_or_end_of_line(MwParser* parser, unsigned position)
{
    position = pw_string_skip_spaces(&parser->current_line, position);
    return (end_of_line(&parser->current_line, position)
            || pw_char_at(&parser->current_line, position) == MW_COMMENT);
}

static PwResult parse_convspec(MwParser* parser, unsigned opening_colon_pos, unsigned* end_pos)
/*
 * Extract conversion specifier starting from `opening_colon_pos` in the `current_line`.
 *
 * On success return string and write `end_pos`.
 *
 * If conversion specified is not detected, return PwNull()
 *
 * On error return PwStatus.
 */
{
    PwValuePtr current_line = &parser->current_line;

    unsigned start_pos = opening_colon_pos + 1;
    unsigned closing_colon_pos;
    if (!pw_strchr(current_line, ':', start_pos, &closing_colon_pos)) {
        return PwNull();
    }
    if (closing_colon_pos == start_pos) {
        // empty conversion specifier
        return PwNull();
    }
    if (!isspace_or_eol_at(current_line, closing_colon_pos + 1)) {
        // not a conversion specifier
        return PwNull();
    }
    PwValue convspec = pw_substr(current_line, start_pos, closing_colon_pos);
    pw_return_if_error(&convspec);

    if (!pw_string_trim(&convspec)) {
        return PwOOM();
    }
    if (!have_custom_parser(parser, &convspec)) {
        // such a conversion specifier is not defined
        return PwNull();
    }
    *end_pos = closing_colon_pos + 1;
    return pw_move(&convspec);
}

static PwResult parse_raw_value(MwParser* parser)
{
    TRACEPOINT();

    PwValue lines = _mw_read_block(parser);
    pw_return_if_error(&lines);

    if (pw_array_length(&lines) > 1) {
        // append one empty line for ending line break
        PWDECL_String(empty_line);
        pw_expect_ok( pw_array_append(&lines, &empty_line) );
    }
    // return concatenated lines
    return pw_array_join('\n', &lines);
}

static PwResult parse_literal_string(MwParser* parser)
/*
 * Parse current block as a literal string.
 */
{
    TRACEPOINT();

    PwValue lines = _mw_read_block(parser);
    pw_return_if_error(&lines);

    // normalize list of lines

    pw_expect_ok( pw_array_dedent(&lines) );

    // drop empty trailing lines
    unsigned len = pw_array_length(&lines);
    while (len--) {{
        PwValue line = pw_array_item(&lines, len);
        if (pw_strlen(&line) != 0) {
            break;
        }
        pw_array_del(&lines, len, len + 1);
    }}

    // append one empty line for ending line break
    if (pw_array_length(&lines) > 1) {
        PWDECL_String(empty_line);
        pw_expect_ok( pw_array_append(&lines, &empty_line) );
    }

    // return concatenated lines
    return pw_array_join('\n', &lines);
}

PwResult _mw_unescape_line(MwParser* parser, PwValuePtr line, unsigned line_number,
                            char32_t quote, unsigned start_pos, unsigned end_pos)
{
    PwValue result = pw_create_empty_string(
        end_pos - start_pos,  // unescaped string can be shorter
        pw_string_char_size(line)
    );
    unsigned pos = start_pos;
    while (pos < end_pos) {
        char32_t chr = pw_char_at(line, pos);
        if (chr == quote) {
            // closing quotation mark detected
            break;
        }
        if (chr != '\\') {
            if (!pw_string_append(&result, chr)) {
                return PwOOM();
            }
        } else {
            // start of escape sequence
            pos++;
            if (pos >= end_pos) {
                if (!pw_string_append(&result, chr)) {  // leave backslash in the result
                    return PwOOM();
                }
                return PwOK();
            }
            bool append_ok = false;
            int hexlen;
            chr = pw_char_at(line, pos);
            switch (chr) {

                // Simple escape sequences
                case '\'':    //  \'   single quote     byte 0x27
                case '"':     //  \"   double quote     byte 0x22
                case '?':     //  \?   question mark    byte 0x3f
                case '\\':    //  \\   backslash        byte 0x5c
                    append_ok = pw_string_append(&result, chr);
                    break;
                case 'a': append_ok = pw_string_append(&result, 0x07); break;  // audible bell
                case 'b': append_ok = pw_string_append(&result, 0x08); break;  // backspace
                case 'f': append_ok = pw_string_append(&result, 0x0c); break;  // form feed
                case 'n': append_ok = pw_string_append(&result, 0x0a); break;  // line feed
                case 'r': append_ok = pw_string_append(&result, 0x0d); break;  // carriage return
                case 't': append_ok = pw_string_append(&result, 0x09); break;  // horizontal tab
                case 'v': append_ok = pw_string_append(&result, 0x0b); break;  // vertical tab

                // Numeric escape sequences
                case 'o': {
                    //  \on{1:3} code unit n... (1-3 octal digits)
                    char32_t v = 0;
                    for (int i = 0; i < 3; i++) {
                        pos++;
                        if (pos >= end_pos) {
                            if (i == 0) {
                                return mw_parser_error2(parser, line_number, pos, "Incomplete octal value");
                            }
                            break;
                        }
                        char32_t c = pw_char_at(line, pos);
                        if ('0' <= c && c <= '7') {
                            v <<= 3;
                            v += c - '0';
                        } else {
                            return mw_parser_error2(parser, line_number, pos, "Bad octal value");
                        }
                    }
                    append_ok = pw_string_append(&result, v);
                    break;
                }
                case 'x':
                    //  \xn{2}   code unit n... (exactly 2 hexadecimal digits are required)
                    hexlen = 2;
                    goto parse_hex_value;

                // Unicode escape sequences
                case 'u':
                    //  \un{4}  code point U+n... (exactly 4 hexadecimal digits are required)
                    hexlen = 4;
                    goto parse_hex_value;
                case 'U':
                    //  \Un{8}  code point U+n... (exactly 8 hexadecimal digits are required)
                    hexlen = 8;

                parse_hex_value: {
                    char32_t v = 0;
                    for (int i = 0; i < hexlen; i++) {
                        pos++;
                        if (pos >= end_pos) {
                            return mw_parser_error2(parser, line_number, pos, "Incomplete hexadecimal value");
                        }
                        char32_t c = pw_char_at(line, pos);
                        if ('0' <= c && c <= '9') {
                            v <<= 4;
                            v += c - '0';
                        } else if ('a' <= c && c <= 'f') {
                            v <<= 4;
                            v += c - 'a' + 10;
                        } else if ('A' <= c && c <= 'F') {
                            v <<= 4;
                            v += c - 'A' + 10;
                        } else {
                            return mw_parser_error2(parser, line_number, pos, "Bad hexadecimal value");
                        }
                    }
                    append_ok = pw_string_append(&result, v);
                    break;
                }
                default:
                    // not a valid escape sequence
                    append_ok = pw_string_append(&result, '\\');
                    if (append_ok) {
                        append_ok = pw_string_append(&result, chr);
                    }
                    break;
            }
            if (!append_ok) {
                return PwOOM();
            }
        }
        pos++;
    }
    return pw_move(&result);
}

static PwResult fold_lines(MwParser* parser, PwValuePtr lines, char32_t quote, PwValuePtr line_numbers)
/*
 * Fold list of lines and return concatenated string.
 *
 * If `quote` is nonzero, unescape lines.
 */
{
    pw_expect_ok( pw_array_dedent(lines) );
    unsigned len = pw_array_length(lines);

    // skip leading empty lines
    unsigned start_i = 0;
    for (; start_i < len; start_i++) {{
        PwValue line = pw_array_item(lines, start_i);
        if (pw_strlen(&line) != 0) {
            break;
        }
    }}
    if (start_i == len) {
        // return empty string
        return PwString();
    }

    // skip trailing empty lines
    unsigned end_i = len;
    for (; end_i; end_i--) {{
        PwValue line = pw_array_item(lines, end_i - 1);
        if (pw_strlen(&line) != 0) {
            break;
        }
    }}
    if (end_i == 0) {
        // return empty string
        return PwString();
    }

    // calculate length of result
    unsigned result_len = end_i - start_i - 1;  // reserve space for separators
    uint8_t char_size = 1;
    for (unsigned i = start_i; i < end_i; i++) {{
        PwValue line = pw_array_item(lines, i);
        result_len += pw_strlen(&line);
        uint8_t cs = pw_string_char_size(&line);
        if (cs > char_size) {
            char_size = cs;
        }
    }}

    // allocate result
    PwValue result = pw_create_empty_string(result_len, char_size);
    pw_return_if_error(&result);

    // concatenate lines
    bool prev_LF = false;
    for (unsigned i = start_i; i < end_i; i++) {{
        PwValue line = pw_array_item(lines, i);
        if (i > start_i) {
            if (pw_strlen(&line) == 0) {
                // treat empty lines as LF
                if (!pw_string_append(&line, '\n')) {
                    return PwOOM();
                }
                prev_LF = true;
            } else {
                if (prev_LF) {
                    // do not append separator if previous line was empty
                    prev_LF = false;
                } else {
                    if (pw_isspace(pw_char_at(&line, 0))) {
                        // do not append separator if the line aleady starts with space
                    } else {
                        if (!pw_string_append(&result, ' ')) {
                            return PwOOM();
                        }
                    }
                }
            }
        }
        if (quote) {
            PwValue line_number = pw_array_item(line_numbers, i);
            PwValue unescaped = _mw_unescape_line(parser, &line, line_number.unsigned_value,
                                                   quote, 0, pw_strlen(&line));
            pw_return_if_error(&unescaped);
            if (!pw_string_append(&result, &unescaped)) {
                return PwOOM();
            }
        } else {
            if (!pw_string_append(&result, &line)) {
                return PwOOM();
            }
        }
    }}
    return pw_move(&result);
}

static PwResult parse_folded_string(MwParser* parser)
{
    TRACEPOINT();

    PwValue lines = _mw_read_block(parser);
    pw_return_if_error(&lines);

    return fold_lines(parser, &lines, 0, nullptr);
}

bool _mw_find_closing_quote(PwValuePtr line, char32_t quote, unsigned start_pos, unsigned* end_pos)
{
    for (;;) {
        if (!pw_strchr(line, quote, start_pos, end_pos)) {
            return false;
        }
        // check if the quotation mark is not escaped
        if (*end_pos && pw_char_at(line, *end_pos - 1) == '\\') {
            // continue searching
            start_pos = *end_pos + 1;
        } else {
            return true;
        }
    }
}

static PwResult parse_quoted_string(MwParser* parser, unsigned opening_quote_pos, unsigned* end_pos)
/*
 * Parse quoted string starting from `opening_quote_pos` in the current line.
 *
 * Write next position after the closing quotation mark to `end_pos`.
 */
{
    TRACEPOINT();

    // Get opening quote. The closing quote should be the same.
    char32_t quote = pw_char_at(&parser->current_line, opening_quote_pos);

    // process first line
    unsigned closing_quote_pos;
    if (_mw_find_closing_quote(&parser->current_line, quote, opening_quote_pos + 1, &closing_quote_pos)) {
        // single-line string
        *end_pos = closing_quote_pos + 1;
        return _mw_unescape_line(parser, &parser->current_line, parser->line_number,
                                  quote, opening_quote_pos + 1, closing_quote_pos);
    }

    unsigned block_indent = opening_quote_pos + 1;

    // make parser read nested block
    unsigned saved_block_indent = parser->block_indent;
    parser->block_indent = block_indent;
    parser->blocklevel++;

    // read block
    PwValue lines = PwArray();
    pw_return_if_error(&lines);

    PwValue line_numbers = PwArray();
    pw_return_if_error(&line_numbers);

    bool closing_quote_detected = false;
    for (;;) {{
        // append line number
        PwValue n = PwUnsigned(parser->line_number);
        pw_expect_ok( pw_array_append(&line_numbers, &n) );

        // append line
        if (_mw_find_closing_quote(&parser->current_line, quote, block_indent, end_pos)) {
            // final line
            PwValue final_line = pw_substr(&parser->current_line, block_indent, *end_pos);
            if (!pw_string_rtrim(&final_line)) {
                return PwOOM();
            }
            pw_expect_ok( pw_array_append(&lines, &final_line) );
            (*end_pos)++;
            closing_quote_detected = true;
            break;
        } else {
            // intermediate line
            PwValue line = pw_substr(&parser->current_line, block_indent, UINT_MAX);
            pw_return_if_error(&line);
            pw_expect_ok( pw_array_append(&lines, &line) );
        }
        // read next line
        PwValue status = _mw_read_block_line(parser);
        if (_mw_end_of_block(&status)) {
            break;
        }
        pw_return_if_error(&status);
    }}

    // finished reading nested block
    parser->block_indent = saved_block_indent;
    parser->blocklevel--;

    if (!closing_quote_detected) {

        static char unterminated[] = "String has no closing quote";

        // the above loop terminated abnormally, need to read next line
        PwValue status = _mw_read_block_line(parser);
        if (_mw_end_of_block(&status)) {
            return mw_parser_error(parser, parser->current_indent, unterminated);
        }
        // check if the line starts with a quote with the same indent as the opening quote
        if (parser->current_indent == opening_quote_pos
            && pw_char_at(&parser->current_line, parser->current_indent) == quote) {

            *end_pos = opening_quote_pos + 1;
        } else {
            return mw_parser_error(parser, parser->current_indent, unterminated);
        }
    }

    // fold and unescape

    return fold_lines(parser, &lines, quote, &line_numbers);
}

static PwResult parse_datetime(MwParser* parser)
/*
 * Parse value date/time starting from block indent in the current line.
 * Return PwDateTime on success, PwStatus on error.
 */
{
    static char bad_datetime[] = "Bad date/time";
    static char32_t allowed_terminators[] = { MW_COMMENT, 0 };

    unsigned start_pos = _mw_get_start_position(parser);
    unsigned end_pos;
    PwValue result = _pw_parse_datetime(&parser->current_line, start_pos, &end_pos, allowed_terminators);
    if (pw_error(&result)) {
        if (result.status_code == PW_ERROR_BAD_DATETIME) {
            return mw_parser_error(parser, start_pos, bad_datetime);
        }
        return pw_move(&result);
    }
    if (_mw_comment_or_end_of_line(parser, end_pos)) {
        return pw_move(&result);
    } else {
        return mw_parser_error(parser, start_pos, bad_datetime);
    }
}

static PwResult parse_timestamp(MwParser* parser)
/*
 * Parse value as timestamp starting from block indent in the current line.
 * Return PwTimestamp on success, PwStatus on error.
 */
{
    static char bad_timestamp[] = "Bad timestamp";
    static char32_t allowed_terminators[] = { MW_COMMENT, 0 };

    unsigned start_pos = _mw_get_start_position(parser);
    unsigned end_pos;
    PwValue result = _pw_parse_timestamp(&parser->current_line, start_pos, &end_pos, allowed_terminators);
    if (pw_error(&result)) {
        if (result.status_code == PW_ERROR_BAD_TIMESTAMP) {
            return mw_parser_error(parser, start_pos, bad_timestamp);
        }
        if (result.status_code == PW_ERROR_NUMERIC_OVERFLOW) {
            return mw_parser_error(parser, start_pos, "Numeric overflow");
        }
        return pw_move(&result);
    }
    if (_mw_comment_or_end_of_line(parser, end_pos)) {
        return pw_move(&result);
    } else {
        return mw_parser_error(parser, end_pos, bad_timestamp);
    }
}

PwResult _mw_parse_number(MwParser* parser, unsigned start_pos, int sign, unsigned* end_pos, char32_t* allowed_terminators)
{
    TRACEPOINT();
    TRACE("start_pos %u", start_pos);

    PwValue result = _pw_parse_number(&parser->current_line, start_pos, sign, end_pos, allowed_terminators);
    if (pw_error(&result)) {
        if (result.status_code == PW_ERROR_BAD_NUMBER) {
            return mw_parser_error(parser, start_pos, "Bad number");
        }
        if (result.status_code == PW_ERROR_NUMERIC_OVERFLOW) {
            return mw_parser_error(parser, start_pos, "Numeric overflow");
        }
    }
    return pw_move(&result);
}

static PwResult parse_list(MwParser* parser)
/*
 * Parse list.
 *
 * Return list value on success.
 * Return nullptr on error.
 */
{
    TRACE_ENTER();

    PwValue result = PwArray();
    pw_return_if_error(&result);

    /*
     * All list items must have the same indent.
     * Save indent of the first item (current one) and check it for subsequent items.
     */
    unsigned item_indent = _mw_get_start_position(parser);

    for (;;) {
        {
            // check if hyphen is followed by space or end of line
            unsigned next_pos = item_indent + 1;
            if (!isspace_or_eol_at(&parser->current_line, next_pos)) {
                return mw_parser_error(parser, item_indent, "Bad list item");
            }

            // parse item as a nested block

            PwValue item = PwNull();
            if (_mw_comment_or_end_of_line(parser, next_pos)) {
                item = parse_nested_block_from_next_line(parser, value_parser_func);
            } else {
                // nested block starts on the same line, increment block position
                next_pos++;
                item = parse_nested_block(parser, next_pos, value_parser_func);
            }
            pw_return_if_error(&item);

            pw_expect_ok( pw_array_append(&result, &item) );

            PwValue status = _mw_read_block_line(parser);
            if (_mw_end_of_block(&status)) {
                break;
            }
            pw_return_if_error(&status);

            if (parser->current_indent != item_indent) {
                return mw_parser_error(parser, parser->current_indent, "Bad indentation of list item");
            }
        }
    }
    TRACE_EXIT();
    return pw_move(&result);
}

static PwResult parse_map(MwParser* parser, PwValuePtr first_key, PwValuePtr convspec_arg, unsigned value_pos)
/*
 * Parse map.
 *
 * Key is already parsed, continue parsing from `value_pos` in the `current_line`.
 *
 * Return map value on success.
 * Return status on error.
 */
{
    TRACE_ENTER();

    PwValue result = PwMap();
    pw_return_if_error(&result);

    PwValue key = pw_clone(first_key);
    PwValue convspec = pw_clone(convspec_arg);

    /*
     * All keys in the map must have the same indent.
     * Save indent of the first key (current one) and check it for subsequent keys.
     */
    unsigned key_indent = _mw_get_start_position(parser);

    for (;;) {
        TRACE("parse value (line %u) from position %u", parser->line_number, value_pos);
        {
            // parse value as a nested block

            MwBlockParserFunc parser_func = value_parser_func;
            if (pw_is_string(&convspec)) {
                parser_func = get_custom_parser(parser, &convspec);
            }
            PwValue value = PwNull();
            if (_mw_comment_or_end_of_line(parser, value_pos)) {
                value = parse_nested_block_from_next_line(parser, parser_func);

            } else {
                value = parse_nested_block(parser, value_pos, parser_func);
            }
            pw_return_if_error(&value);

            pw_expect_ok( pw_map_update(&result, &key, &value) );
        }
        TRACE("parse next key");
        {
            pw_destroy(&key);
            pw_destroy(&convspec);

            PwValue status = _mw_read_block_line(parser);
            if (_mw_end_of_block(&status)) {
                TRACE("end of map");
                break;
            }
            pw_return_if_error(&status);

            if (parser->current_indent != key_indent) {
                return mw_parser_error(parser, parser->current_indent, "Bad indentation of map key");
            }

            key = parse_value(parser, &value_pos, &convspec);
            pw_return_if_error(&key);
        }
    }
    TRACE_EXIT();
    return pw_move(&result);
}

static PwResult is_kv_separator(MwParser* parser, unsigned colon_pos,
                                PwValuePtr convspec_out, unsigned *value_pos)
/*
 * Return PwBool(true) if colon_pos is followed by end of line, space, or conversion specifier.
 * Write conversion specifier to `convspec_out` if value is followed by conversion specifier.
 * Write position of value to value_pos.
 */
{
    PwValuePtr current_line = &parser->current_line;

    unsigned next_pos = colon_pos + 1;

    if (end_of_line(current_line, next_pos)) {
        *value_pos = next_pos;
        return PwBool(true);
    }
    char32_t chr = pw_char_at(current_line, next_pos);
    if (isspace(chr)) {
        *value_pos = next_pos + 1;  // value should be separated from key by at least one space
        next_pos = pw_string_skip_spaces(current_line, next_pos);
        // cannot be end of line here because current line is R-trimmed and EOL is already checked
        chr = pw_char_at(current_line, next_pos);
        if (chr != ':') {
            // separator without conversion specifier
            return PwBool(true);
        }
    } else if (chr != ':') {
        // key not followed immediately by conversion specifier -> not a separator
        return PwBool(false);
    }

    // try parsing conversion specifier
    // value_pos will be updated only if conversion specifier is valid
    PwValue convspec = parse_convspec(parser, next_pos, value_pos);
    pw_return_if_error(&convspec);

    if (pw_is_string(&convspec)) {
        if (convspec_out) {
            *convspec_out = pw_move(&convspec);
        }
        return PwBool(true);
    }

    // bad conversion specifier -> not a separator
    return PwBool(false);
}

static PwResult check_value_end(MwParser* parser, PwValuePtr value, unsigned end_pos,
                                unsigned* nested_value_pos, PwValuePtr convspec_out)
/*
 * Helper function for parse_value.
 *
 * Check if value ends with key-value separator and parse map.
 * If not, check if end_pos points to end of line or comment.
 *
 * If `nested_value_pos` is provided, the value is _expected_ to be a map key
 * and _must_ end with key-value separator.
 *
 * On success return parsed value.
 * If `nested_value_pos' is not null, write position of the next char after colon to it
 * and write conversion specifier to `convspec_out` if value is followed by conversion specifier.
 *
 * Read next line if nothing to parse on the current_line.
 *
 * Return cloned value.
 */
{
    //make sure value is not an error
    if (pw_error(value)) {
        return pw_clone(value);
    }

    end_pos = pw_string_skip_spaces(&parser->current_line, end_pos);
    if (end_of_line(&parser->current_line, end_pos)) {
        if (nested_value_pos) {
            return mw_parser_error(parser, end_pos, "Map key expected");
        }
        // read next line
        PwValue status = _mw_read_block_line(parser);
        if (!_mw_end_of_block(&status)) {
            pw_return_if_error(&status);
        }
        return pw_clone(value);
    }

    char32_t chr = pw_char_at(&parser->current_line, end_pos);
    if (chr == ':') {
        // check key-value separator
        PwValue convspec = PwNull();
        unsigned value_pos;
        PwValue kvs = is_kv_separator(parser, end_pos, &convspec, &value_pos);
        pw_return_if_error(&kvs);

        if (kvs.bool_value) {
            // found key-value separator
            if (nested_value_pos) {
                // it was anticipated, just return the value
                *nested_value_pos = value_pos;
                *convspec_out = pw_move(&convspec);
                return pw_clone(value);
            }
            // parse map
            PwValue first_key = pw_clone(value);
            return parse_map(parser, &first_key, &convspec, value_pos);
        }
        return mw_parser_error(parser, end_pos + 1, "Bad character encountered");
    }

    if (chr != MW_COMMENT) {
        return mw_parser_error(parser, end_pos, "Bad character encountered");
    }

    // read next line
    PwValue status = _mw_read_block_line(parser);
    if (!_mw_end_of_block(&status)) {
        pw_return_if_error(&status);
    }
    return pw_clone(value);
}

static PwResult parse_value(MwParser* parser, unsigned* nested_value_pos, PwValuePtr convspec_out)
/*
 * Parse value starting from `current_line[block_indent]` .
 *
 * If `nested_value_pos` is provided, the value is _expected_ to be a map key
 * and _must_ end with colon or include a colon if it's a literal strings.
 *
 * On success return parsed value.
 * If `nested_value_pos' is provided, write position of the next char after colon to it
 * and write conversion specifier to `convspec_out` if it's followed by conversion specifier.
 *
 * On error return status and set `parser->result["error"]`.
 */
{
    TRACEPOINT();

    unsigned start_pos = _mw_get_start_position(parser);

    // Analyze first character.
    char32_t chr = pw_char_at(&parser->current_line, start_pos);

    // first, check if value starts with colon that may denote conversion specifier

    if (chr == ':') {
        // this might be conversion specifier
        if (nested_value_pos) {
            // we expect map key, and map keys cannot start with colon
            // because they would look same as conversion specifier
            return mw_parser_error(parser, start_pos, "Map key expected and it cannot start with colon");
        }
        unsigned value_pos;
        PwValue convspec = parse_convspec(parser, start_pos, &value_pos);
        pw_return_if_error(&convspec);

        if (!pw_is_string(&convspec)) {
            // not a conversion specifier
            return parse_literal_string(parser);
        }
        // we have conversion specifier
        if (end_of_line(&parser->current_line, value_pos)) {

            // conversion specifier is followed by LF
            // continue parsing CURRENT block from next line
            PwValue status = _mw_read_block_line(parser);
            if (_mw_end_of_block(&status)) {
                return mw_parser_error(parser, parser->current_indent, "Empty block");
            }
            pw_return_if_error(&status);

            // call parser function
            MwBlockParserFunc parser_func = get_custom_parser(parser, &convspec);
            return parser_func(parser);

        } else {
            // value is on the same line, parse it as nested block
            return parse_nested_block(
                parser, value_pos, get_custom_parser(parser, &convspec)
            );
        }
    }

    // other values can be map keys

    // check for dash

    if (chr == '-') {
        unsigned next_pos = start_pos + 1;
        char32_t next_chr = pw_char_at(&parser->current_line, next_pos);

        // if followed by digit, it's a number
        if ('0' <= next_chr && next_chr <= '9') {
            unsigned end_pos;
            PwValue number = _mw_parse_number(parser, next_pos, -1, &end_pos, number_terminators);
            return check_value_end(parser, &number, end_pos, nested_value_pos, convspec_out);
        }
        // if followed by space or end of line, that's a list item
        if (isspace_or_eol_at(&parser->current_line, next_pos)) {
            if (nested_value_pos) {
                return mw_parser_error(parser, start_pos, "Map key expected and it cannot be a list");
            }
            // yes, it's a list item
            return parse_list(parser);
        }
        // otherwise, it's a literal string or map
        goto parse_literal_string_or_map;
    }

    // check for quoted string

    if (chr == '"' || chr == '\'') {
        // quoted string
        unsigned start_line = parser->line_number;
        unsigned end_pos;
        PwValue str = parse_quoted_string(parser, start_pos, &end_pos);
        pw_return_if_error(&str);

        unsigned end_line = parser->line_number;
        if (end_line == start_line) {
            // single-line string can be a map key
            return check_value_end(parser, &str, end_pos, nested_value_pos, convspec_out);
        } else if (_mw_comment_or_end_of_line(parser, end_pos)) {
            // multi-line string cannot be a key
            return pw_move(&str);
        } else {
            return mw_parser_error(parser, end_pos, "Bad character after quoted string");
        }
    }

    // check for reserved keywords

    TRACE("trying reserved keywords");
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "null")) {
        PwValue null_value = PwNull();
        return check_value_end(parser, &null_value, start_pos + 4, nested_value_pos, convspec_out);
    }
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 4, "true")) {
        PwValue true_value = PwBool(true);
        return check_value_end(parser, &true_value, start_pos + 4, nested_value_pos, convspec_out);
    }
    if (pw_substring_eq(&parser->current_line, start_pos, start_pos + 5, "false")) {
        PwValue false_value = PwBool(false);
        return check_value_end(parser, &false_value, start_pos + 5, nested_value_pos, convspec_out);
    }

    // try parsing number

    TRACE("not a keyword, trying number");
    if (chr == '+') {
        char32_t next_chr = pw_char_at(&parser->current_line, start_pos + 1);
        if ('0' <= next_chr && next_chr <= '9') {
            start_pos++;
            chr = next_chr;
        }
    }
    if ('0' <= chr && chr <= '9') {
        unsigned end_pos;
        PwValue number = _mw_parse_number(parser, start_pos, 1, &end_pos, number_terminators);
        return check_value_end(parser, &number, end_pos, nested_value_pos, convspec_out);
    }
    TRACE("not a number, pasring literal string or map");

parse_literal_string_or_map:

    // look for key-value separator
    for (unsigned pos = start_pos;;) {
        unsigned colon_pos;
        if (!pw_strchr(&parser->current_line, ':', pos, &colon_pos)) {
            break;
        }
        PwValue convspec = PwNull();
        unsigned value_pos;
        PwValue kvs = is_kv_separator(parser, colon_pos, &convspec, &value_pos);
        pw_return_if_error(&kvs);

        if (kvs.bool_value) {
            // found key-value separator, get key
            PwValue key = pw_substr(&parser->current_line, start_pos, colon_pos);
            pw_return_if_error(&key);

            // strip trailing spaces
            if (!pw_string_rtrim(&key)) {
                return PwOOM();
            }

            if (nested_value_pos) {
                // key was anticipated, simply return it
                *nested_value_pos = value_pos;
                *convspec_out = pw_move(&convspec);
                return pw_move(&key);
            }

            // parse map
            return parse_map(parser, &key, &convspec, value_pos);
        }
        pos = colon_pos + 1;
    }

    // separator not found

    if (nested_value_pos) {
        // expecting key, but it's a bare literal string
        return mw_parser_error(parser, parser->current_indent, "Not a key");
    }
    return parse_literal_string(parser);
}

static PwResult value_parser_func(MwParser* parser)
{
    return parse_value(parser, nullptr, nullptr);
}

PwResult mw_parse(PwValuePtr markup)
{
    [[ gnu::cleanup(mw_delete_parser) ]] MwParser* parser = mw_create_parser(markup);
    if (!parser) {
        return PwOOM();
    }
    // read first line to prepare for parsing and to detect EOF
    PwValue status = _mw_read_block_line(parser);
    if (_mw_end_of_block(&status) && parser->eof) {
        return PwStatus(PW_ERROR_EOF);
    }
    pw_return_if_error(&status);

    // parse top-level value
    PwValue result = value_parser_func(parser);
    pw_return_if_error(&result);

    // make sure markup has no more data
    status = _mw_read_block_line(parser);
    if (parser->eof) {
        // all right, no op
    } else {
        pw_return_if_error(&status);
        return mw_parser_error(parser, parser->current_indent, "Extra data after parsed value");
    }
    return pw_move(&result);
}
