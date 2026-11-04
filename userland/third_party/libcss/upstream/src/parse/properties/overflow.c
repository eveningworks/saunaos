/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2012 Michael Drake <tlsa@netsurf-browser.org>
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/* Changed for toy-os, 2026-10-16: TWO keywords are overflow-x then
 * overflow-y (CSS Overflow 3, "overflow: hidden auto"); one is both,
 * as before. A second token that is not one of them is left for the
 * declaration's trailing-junk check, which discards it. */
static bool overflow_keyword(css_language *c, const css_token *token,
		uint16_t *value)
{
	bool match;

	if (token == NULL || token->type != CSS_TOKEN_IDENT)
		return false;
	if (lwc_string_caseless_isequal(token->idata,
			c->strings[VISIBLE], &match) == lwc_error_ok && match)
		*value = OVERFLOW_VISIBLE;
	else if (lwc_string_caseless_isequal(token->idata,
			c->strings[HIDDEN], &match) == lwc_error_ok && match)
		*value = OVERFLOW_HIDDEN;
	else if (lwc_string_caseless_isequal(token->idata,
			c->strings[SCROLL], &match) == lwc_error_ok && match)
		*value = OVERFLOW_SCROLL;
	else if (lwc_string_caseless_isequal(token->idata,
			c->strings[AUTO], &match) == lwc_error_ok && match)
		*value = OVERFLOW_AUTO;
	else
		return false;
	return true;
}

/**
 * Parse overflow shorthand
 *
 * \param c	  Parsing context
 * \param vector  Vector of tokens to process
 * \param ctx	  Pointer to vector iteration context
 * \param result  Pointer to location to receive resulting style
 * \return CSS_OK on success,
 *	   CSS_NOMEM on memory exhaustion,
 *	   CSS_INVALID if the input is not valid
 *
 * Post condition: \a *ctx is updated with the next token to process
 *		   If the input is invalid, then \a *ctx remains unchanged.
 */
css_error css__parse_overflow(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	css_error error1, error2 = CSS_OK;
	const css_token *token;
	enum flag_value flag_value;
	uint16_t x, y;

	token = parserutils_vector_iterate(vector, ctx);
	if ((token == NULL) || ((token->type != CSS_TOKEN_IDENT))) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	flag_value = get_css_flag_value(c, token);

	if (flag_value != FLAG_VALUE__NONE) {
		error1 = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_OVERFLOW_X);
		error2 = css_stylesheet_style_flag_value(result, flag_value,
				CSS_PROP_OVERFLOW_Y);

	} else if (overflow_keyword(c, token, &x)) {
		y = x;
		consumeWhitespace(vector, ctx);
		if (overflow_keyword(c, parserutils_vector_peek(vector, *ctx), &y))
			parserutils_vector_iterate(vector, ctx);
		error1 = css__stylesheet_style_appendOPV(result,
				CSS_PROP_OVERFLOW_X, 0, x);
		error2 = css__stylesheet_style_appendOPV(result,
				CSS_PROP_OVERFLOW_Y, 0, y);

	} else {
		error1 = CSS_INVALID;
	}

	if (error2 != CSS_OK)
		error1 = error2;

	if (error1 != CSS_OK)
		*ctx = orig_ctx;

	return error1;
}

