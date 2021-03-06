/*	$Id: term.c,v 1.148 2010/06/19 20:46:28 kristaps Exp $ */
/*
 * Copyright (c) 2008, 2009 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mandoc.h"
#include "chars.h"
#include "out.h"
#include "term.h"
#include "man.h"
#include "mdoc.h"
#include "main.h"

static	void		  spec(struct termp *, const char *, size_t);
static	void		  res(struct termp *, const char *, size_t);
static	void		  buffera(struct termp *, const char *, size_t);
static	void		  bufferc(struct termp *, char);
static	void		  adjbuf(struct termp *p, size_t);
static	void		  encode(struct termp *, const char *, size_t);


void
term_free(struct termp *p)
{

	if (p->buf)
		free(p->buf);
	if (p->symtab)
		chars_free(p->symtab);

	free(p);
}


void
term_begin(struct termp *p, term_margin head, 
		term_margin foot, const void *arg)
{

	p->headf = head;
	p->footf = foot;
	p->argf = arg;
	(*p->begin)(p);
}


void
term_end(struct termp *p)
{

	(*p->end)(p);
}


struct termp *
term_alloc(enum termenc enc)
{
	struct termp	*p;

	p = calloc(1, sizeof(struct termp));
	if (NULL == p) {
		perror(NULL);
		exit(EXIT_FAILURE);
	}

	p->tabwidth = 5;
	p->enc = enc;
	p->defrmargin = 78;
	return(p);
}


/*
 * Flush a line of text.  A "line" is loosely defined as being something
 * that should be followed by a newline, regardless of whether it's
 * broken apart by newlines getting there.  A line can also be a
 * fragment of a columnar list (`Bl -tag' or `Bl -column'), which does
 * not have a trailing newline.
 *
 * The following flags may be specified:
 *
 *  - TERMP_NOLPAD: when beginning to write the line, don't left-pad the
 *    offset value.  This is useful when doing columnar lists where the
 *    prior column has right-padded.
 *
 *  - TERMP_NOBREAK: this is the most important and is used when making
 *    columns.  In short: don't print a newline and instead pad to the
 *    right margin.  Used in conjunction with TERMP_NOLPAD.
 *
 *  - TERMP_TWOSPACE: when padding, make sure there are at least two
 *    space characters of padding.  Otherwise, rather break the line.
 *
 *  - TERMP_DANGLE: don't newline when TERMP_NOBREAK is specified and
 *    the line is overrun, and don't pad-right if it's underrun.
 *
 *  - TERMP_HANG: like TERMP_DANGLE, but doesn't newline when
 *    overruning, instead save the position and continue at that point
 *    when the next invocation.
 *
 *  In-line line breaking:
 *
 *  If TERMP_NOBREAK is specified and the line overruns the right
 *  margin, it will break and pad-right to the right margin after
 *  writing.  If maxrmargin is violated, it will break and continue
 *  writing from the right-margin, which will lead to the above scenario
 *  upon exit.  Otherwise, the line will break at the right margin.
 */
void
term_flushln(struct termp *p)
{
	int		 i;     /* current input position in p->buf */
	size_t		 vis;   /* current visual position on output */
	size_t		 vbl;   /* number of blanks to prepend to output */
	size_t		 vend;	/* end of word visual position on output */
	size_t		 bp;    /* visual right border position */
	int		 j;     /* temporary loop index */
	int		 jhy;	/* last hyphen before line overflow */
	size_t		 maxvis, mmax;

	/*
	 * First, establish the maximum columns of "visible" content.
	 * This is usually the difference between the right-margin and
	 * an indentation, but can be, for tagged lists or columns, a
	 * small set of values. 
	 */

	assert(p->offset < p->rmargin);

	maxvis = (int)(p->rmargin - p->offset) - p->overstep < 0 ?
		/* LINTED */ 
		0 : p->rmargin - p->offset - p->overstep;
	mmax = (int)(p->maxrmargin - p->offset) - p->overstep < 0 ?
		/* LINTED */
		0 : p->maxrmargin - p->offset - p->overstep;

	bp = TERMP_NOBREAK & p->flags ? mmax : maxvis;

	/*
	 * Indent the first line of a paragraph.
	 */
	vbl = p->flags & TERMP_NOLPAD ? 0 : p->offset;

	/* 
	 * FIXME: if bp is zero, we still output the first word before
	 * breaking the line.
	 */

	vis = vend = i = 0;
	while (i < (int)p->col) {

		/*
		 * Handle literal tab characters.
		 */
		for (j = i; j < (int)p->col; j++) {
			if ('\t' != p->buf[j])
				break;
			vend = (vis/p->tabwidth+1)*p->tabwidth;
			vbl += vend - vis;
			vis = vend;
		}

		/*
		 * Count up visible word characters.  Control sequences
		 * (starting with the CSI) aren't counted.  A space
		 * generates a non-printing word, which is valid (the
		 * space is printed according to regular spacing rules).
		 */

		/* LINTED */
		for (jhy = 0; j < (int)p->col; j++) {
			if ((j && ' ' == p->buf[j]) || '\t' == p->buf[j])
				break;
			if (8 != p->buf[j]) {
				if (vend > vis && vend < bp &&
				    ASCII_HYPH == p->buf[j])
					jhy = j;
				vend++;
			} else
				vend--;
		}

		/*
		 * Find out whether we would exceed the right margin.
		 * If so, break to the next line.
		 */
		if (vend > bp && 0 == jhy && vis > 0) {
			vend -= vis;
			(*p->endline)(p);
			if (TERMP_NOBREAK & p->flags) {
				p->viscol = p->rmargin;
				(*p->advance)(p, p->rmargin);
				vend += p->rmargin - p->offset;
			} else {
				p->viscol = 0;
				vbl = p->offset;
			}

			/* Remove the p->overstep width. */

			bp += (int)/* LINTED */
				p->overstep;
			p->overstep = 0;
		}

		/*
		 * Skip leading tabs, they were handled above.
		 */
		while (i < (int)p->col && '\t' == p->buf[i])
			i++;

		/* Write out the [remaining] word. */
		for ( ; i < (int)p->col; i++) {
			if (vend > bp && jhy > 0 && i > jhy)
				break;
			if ('\t' == p->buf[i])
				break;
			if (' ' == p->buf[i]) {
				while (' ' == p->buf[i]) {
					vbl++;
					i++;
				}
				break;
			}
			if (ASCII_NBRSP == p->buf[i]) {
				vbl++;
				continue;
			}

			/*
			 * Now we definitely know there will be
			 * printable characters to output,
			 * so write preceding white space now.
			 */
			if (vbl) {
				(*p->advance)(p, vbl);
				p->viscol += vbl;
				vbl = 0;
			}

			if (ASCII_HYPH == p->buf[i])
				(*p->letter)(p, '-');
			else
				(*p->letter)(p, p->buf[i]);

			p->viscol += 1;
		}
		vend += vbl;
		vis = vend;
	}

	p->col = 0;
	p->overstep = 0;

	if ( ! (TERMP_NOBREAK & p->flags)) {
		p->viscol = 0;
		(*p->endline)(p);
		return;
	}

	if (TERMP_HANG & p->flags) {
		/* We need one blank after the tag. */
		p->overstep = /* LINTED */
			vis - maxvis + 1;

		/*
		 * Behave exactly the same way as groff:
		 * If we have overstepped the margin, temporarily move
		 * it to the right and flag the rest of the line to be
		 * shorter.
		 * If we landed right at the margin, be happy.
		 * If we are one step before the margin, temporarily
		 * move it one step LEFT and flag the rest of the line
		 * to be longer.
		 */
		if (p->overstep >= -1) {
			assert((int)maxvis + p->overstep >= 0);
			/* LINTED */
			maxvis += p->overstep;
		} else
			p->overstep = 0;

	} else if (TERMP_DANGLE & p->flags)
		return;

	/* Right-pad. */
	if (maxvis > vis + /* LINTED */
			((TERMP_TWOSPACE & p->flags) ? 1 : 0)) {
		p->viscol += maxvis - vis;
		(*p->advance)(p, maxvis - vis);
		vis += (maxvis - vis);
	} else {	/* ...or newline break. */
		(*p->endline)(p);
		p->viscol = p->rmargin;
		(*p->advance)(p, p->rmargin);
	}
}


/* 
 * A newline only breaks an existing line; it won't assert vertical
 * space.  All data in the output buffer is flushed prior to the newline
 * assertion.
 */
void
term_newln(struct termp *p)
{

	p->flags |= TERMP_NOSPACE;
	if (0 == p->col && 0 == p->viscol) {
		p->flags &= ~TERMP_NOLPAD;
		return;
	}
	term_flushln(p);
	p->flags &= ~TERMP_NOLPAD;
}


/*
 * Asserts a vertical space (a full, empty line-break between lines).
 * Note that if used twice, this will cause two blank spaces and so on.
 * All data in the output buffer is flushed prior to the newline
 * assertion.
 */
void
term_vspace(struct termp *p)
{

	term_newln(p);
	p->viscol = 0;
	(*p->endline)(p);
}


static void
spec(struct termp *p, const char *word, size_t len)
{
	const char	*rhs;
	size_t		 sz;

	rhs = chars_a2ascii(p->symtab, word, len, &sz);
	if (rhs) 
		encode(p, rhs, sz);
}


static void
res(struct termp *p, const char *word, size_t len)
{
	const char	*rhs;
	size_t		 sz;

	rhs = chars_a2res(p->symtab, word, len, &sz);
	if (rhs)
		encode(p, rhs, sz);
}


void
term_fontlast(struct termp *p)
{
	enum termfont	 f;

	f = p->fontl;
	p->fontl = p->fontq[p->fonti];
	p->fontq[p->fonti] = f;
}


void
term_fontrepl(struct termp *p, enum termfont f)
{

	p->fontl = p->fontq[p->fonti];
	p->fontq[p->fonti] = f;
}


void
term_fontpush(struct termp *p, enum termfont f)
{

	assert(p->fonti + 1 < 10);
	p->fontl = p->fontq[p->fonti];
	p->fontq[++p->fonti] = f;
}


const void *
term_fontq(struct termp *p)
{

	return(&p->fontq[p->fonti]);
}


enum termfont
term_fonttop(struct termp *p)
{

	return(p->fontq[p->fonti]);
}


void
term_fontpopq(struct termp *p, const void *key)
{

	while (p->fonti >= 0 && key != &p->fontq[p->fonti])
		p->fonti--;
	assert(p->fonti >= 0);
}


void
term_fontpop(struct termp *p)
{

	assert(p->fonti);
	p->fonti--;
}


/*
 * Handle pwords, partial words, which may be either a single word or a
 * phrase that cannot be broken down (such as a literal string).  This
 * handles word styling.
 */
void
term_word(struct termp *p, const char *word)
{
	const char	*sv, *seq;
	int		 sz;
	size_t		 ssz;
	enum roffdeco	 deco;

	sv = word;

	if (word[0] && '\0' == word[1])
		switch (word[0]) {
		case('.'):
			/* FALLTHROUGH */
		case(','):
			/* FALLTHROUGH */
		case(';'):
			/* FALLTHROUGH */
		case(':'):
			/* FALLTHROUGH */
		case('?'):
			/* FALLTHROUGH */
		case('!'):
			/* FALLTHROUGH */
		case(')'):
			/* FALLTHROUGH */
		case(']'):
			if ( ! (TERMP_IGNDELIM & p->flags))
				p->flags |= TERMP_NOSPACE;
			break;
		default:
			break;
		}

	if ( ! (TERMP_NOSPACE & p->flags)) {
		bufferc(p, ' ');
		if (TERMP_SENTENCE & p->flags)
			bufferc(p, ' ');
	}

	if ( ! (p->flags & TERMP_NONOSPACE))
		p->flags &= ~TERMP_NOSPACE;

	p->flags &= ~TERMP_SENTENCE;

	/* FIXME: use strcspn. */

	while (*word) {
		if ('\\' != *word) {
			encode(p, word, 1);
			word++;
			continue;
		}

		seq = ++word;
		sz = a2roffdeco(&deco, &seq, &ssz);

		switch (deco) {
		case (DECO_RESERVED):
			res(p, seq, ssz);
			break;
		case (DECO_SPECIAL):
			spec(p, seq, ssz);
			break;
		case (DECO_BOLD):
			term_fontrepl(p, TERMFONT_BOLD);
			break;
		case (DECO_ITALIC):
			term_fontrepl(p, TERMFONT_UNDER);
			break;
		case (DECO_ROMAN):
			term_fontrepl(p, TERMFONT_NONE);
			break;
		case (DECO_PREVIOUS):
			term_fontlast(p);
			break;
		default:
			break;
		}

		word += sz;
		if (DECO_NOSPACE == deco && '\0' == *word)
			p->flags |= TERMP_NOSPACE;
	}

	/* 
	 * Note that we don't process the pipe: the parser sees it as
	 * punctuation, but we don't in terms of typography.
	 */
	if (sv[0] && 0 == sv[1])
		switch (sv[0]) {
		case('('):
			/* FALLTHROUGH */
		case('['):
			p->flags |= TERMP_NOSPACE;
			break;
		default:
			break;
		}
}


static void
adjbuf(struct termp *p, size_t sz)
{

	if (0 == p->maxcols)
		p->maxcols = 1024;
	while (sz >= p->maxcols)
		p->maxcols <<= 2;

	p->buf = realloc(p->buf, p->maxcols);
	if (NULL == p->buf) {
		perror(NULL);
		exit(EXIT_FAILURE);
	}
}


static void
buffera(struct termp *p, const char *word, size_t sz)
{

	if (p->col + sz >= p->maxcols) 
		adjbuf(p, p->col + sz);

	memcpy(&p->buf[(int)p->col], word, sz);
	p->col += sz;
}


static void
bufferc(struct termp *p, char c)
{

	if (p->col + 1 >= p->maxcols)
		adjbuf(p, p->col + 1);

	p->buf[(int)p->col++] = c;
}


static void
encode(struct termp *p, const char *word, size_t sz)
{
	enum termfont	  f;
	int		  i;

	/*
	 * Encode and buffer a string of characters.  If the current
	 * font mode is unset, buffer directly, else encode then buffer
	 * character by character.
	 */

	if (TERMFONT_NONE == (f = term_fonttop(p))) {
		buffera(p, word, sz);
		return;
	}

	for (i = 0; i < (int)sz; i++) {
		if ( ! isgraph((u_char)word[i])) {
			bufferc(p, word[i]);
			continue;
		}

		if (TERMFONT_UNDER == f)
			bufferc(p, '_');
		else
			bufferc(p, word[i]);

		bufferc(p, 8);
		bufferc(p, word[i]);
	}
}


size_t
term_vspan(const struct roffsu *su)
{
	double		 r;

	switch (su->unit) {
	case (SCALE_CM):
		r = su->scale * 2;
		break;
	case (SCALE_IN):
		r = su->scale * 6;
		break;
	case (SCALE_PC):
		r = su->scale;
		break;
	case (SCALE_PT):
		r = su->scale / 8;
		break;
	case (SCALE_MM):
		r = su->scale / 1000;
		break;
	case (SCALE_VS):
		r = su->scale;
		break;
	default:
		r = su->scale - 1;
		break;
	}

	if (r < 0.0)
		r = 0.0;
	return(/* LINTED */(size_t)
			r);
}


size_t
term_hspan(const struct roffsu *su)
{
	double		 r;

	/* XXX: CM, IN, and PT are approximations. */

	switch (su->unit) {
	case (SCALE_CM):
		r = 4 * su->scale;
		break;
	case (SCALE_IN):
		/* XXX: this is an approximation. */
		r = 10 * su->scale;
		break;
	case (SCALE_PC):
		r = (10 * su->scale) / 6;
		break;
	case (SCALE_PT):
		r = (10 * su->scale) / 72;
		break;
	case (SCALE_MM):
		r = su->scale / 1000; /* FIXME: double-check. */
		break;
	case (SCALE_VS):
		r = su->scale * 2 - 1; /* FIXME: double-check. */
		break;
	default:
		r = su->scale;
		break;
	}

	if (r < 0.0)
		r = 0.0;
	return((size_t)/* LINTED */
			r);
}


