#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <pw.h>

#define MW_MAX_RECURSION_DEPTH  100

#define MW_COMMENT  '#'

typedef struct {
    /*
     * Parse status.
     */
    unsigned line_number;
    unsigned position;
} MwStatusData;

#define _mw_status_data_ptr(value)  ((MwStatusData*) _pw_get_data_ptr((value), PwTypeId_MwStatus))


extern PwTypeId PwTypeId_MwStatus;
/*
 * Type ID for MwStatus value.
 */

/*
 * MW error codes
 */
extern uint16_t MW_END_OF_BLOCK;  // for internal use
extern uint16_t MW_PARSE_ERROR;

typedef struct  {
    _PwValue  markup;
    _PwValue  current_line;
    unsigned  current_indent;  // measured indentation of current line
    unsigned  line_number;
    unsigned  block_indent;    // indent of current block
    unsigned  blocklevel;      // recursion level
    unsigned  max_blocklevel;
    unsigned  json_depth;      // recursion level for JSON
    unsigned  max_json_depth;
    bool      skip_comments;   // initially true to skip leading comments in the block
    bool      eof;
    _PwValue  custom_parsers;
} MwParser;


MwParser* mw_create_parser(PwValuePtr markup);
/*
 * Create parser for `markup` which can be either File, StringIO, or any other value
 * that supports line reader interface. See PetWay library.
 *
 * This function invokes pw_start_read_lines for markup.
 *
 * Return parser on success or nullptr if out of memory.
 */

void mw_delete_parser(MwParser** parser_ptr);
/*
 * Delete parser. The format of the argument is natural for gnu::cleanup attribute.
 */

typedef PwResult (*MwBlockParserFunc)(MwParser* parser);

PwResult mw_set_custom_parser(MwParser* parser, char* convspec, MwBlockParserFunc parser_func);
/*
 * Set custom parser function for `convspec`.
 */

PwResult mw_parse(PwValuePtr markup);
/*
 * Parse `markup`.
 *
 * Return parsed value or error.
 */

PwResult mw_parse_json(PwValuePtr markup);
/*
 * Parse `markup` as pure JSON.
 *
 * Return parsed value or error.
 */

PwResult _mw_json_parser_func(MwParser* parser);
/*
 * JSON parser function for MW :json: conversion specifier.
 */

PwResult _mw_read_block_line(MwParser* parser);
/*
 * Read line belonging to a block, until indent is less than `block_indent`.
 * Skip comments with indentation less than `block_indent`.
 *
 * Return success if line is read, MW_END_OF_BLOCK if there's no more lines
 * in the block, or any other error.
 */

bool _mw_end_of_block(PwValuePtr status);
/*
 * Return true if status is MW_END_OF_BLOCK
 */

PwResult _mw_read_block(MwParser* parser);
/*
 * Read lines starting from current_line till the end of block.
 */

unsigned _mw_get_start_position(MwParser* parser);
/*
 * Return position of the first non-space character in the current block.
 * The block may start inside `current_line` for nested values of list or map.
 */

bool _mw_comment_or_end_of_line(MwParser* parser, unsigned position);
/*
 * Check if current line ends at position or contains comment.
 */

PwResult _mw_parser_error(MwParser* parser, char* source_file_name, unsigned source_line_number,
                           unsigned line_number, unsigned char_pos, char* description, ...);
/*
 * Set error in parser->status and return MW_PARSE_ERROR.
 */

#define mw_parser_error2(parser, line_number, char_pos, description, ...)  \
    _mw_parser_error((parser), __FILE__, __LINE__, (line_number),  \
                      (char_pos), (description) __VA_OPT__(,) __VA_ARGS__)

#define mw_parser_error(parser, char_pos, description, ...)  \
    mw_parser_error2((parser), (parser)->line_number,  \
                      (char_pos), (description) __VA_OPT__(,) __VA_ARGS__)

bool _mw_find_closing_quote(PwValuePtr line, char32_t quote, unsigned start_pos, unsigned* end_pos);
/*
 * Search for closing quotation mark in escaped line.
 * If found, write its position to `end_pos` and return true;
 */

PwResult _mw_unescape_line(MwParser* parser, PwValuePtr line, unsigned line_number,
                            char32_t quote, unsigned start_pos, unsigned end_pos);
/*
 * Process escaped characters in the `line` from `start_pos` to `end_pos`.
 */

PwResult _mw_parse_number(MwParser* parser, unsigned start_pos, int sign, unsigned* end_pos, char32_t* allowed_terminators);
/*
 * Parse number, either integer or float.
 * `start_pos` points to the first digit in the `current_line`.
 *
 * Leading zeros in a non-zero decimal numbers are not allowed to avoid ambiguity.
 *
 * Optional single quote (') or underscores can be used as separators.
 *
 * Return numeric value on success. Set `end_pos` to a point where conversion has stopped.
 */

PwResult _mw_parse_json_value(MwParser* parser, unsigned start_pos, unsigned* end_pos);
/*
 * Parse JSON value starting from `start_pos`.
 * On success write position where parsing stopped to `end_pos`.
 */

#ifdef __cplusplus
}
#endif
