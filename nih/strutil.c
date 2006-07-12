/* libnih
 *
 * strutil.c - useful string utility functions
 *
 * Copyright © 2006 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <stdio.h>
#include <stdarg.h>

#include <nih/macros.h>
#include <nih/alloc.h>

#include "strutil.h"


/**
 * nih_sprintf:
 * @parent: parent block of allocation,
 * @format: format string.
 *
 * Writes a new string according to @format as %sprintf, except that the
 * string is allocated using #nih_alloc.
 *
 * If @parent is not %NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned string will be freed too.  If you have clean-up
 * that would need to be run, you can assign a destructor function using
 * the #nih_alloc_set_destructor function.
 *
 * Returns: newly allocated string or %NULL.
 **/
char *
nih_sprintf (void       *parent,
	     const char *format,
	     ...)
{
	char    *str;
	va_list  args;

	va_start (args, format);
	str = nih_vsprintf (parent, format, args);
	va_end (args);

	return str;
}

/**
 * nih_vsprintf:
 * @parent: parent block of allocation,
 * @format: format string,
 * @args: arguments to format string.
 *
 * Writes a new string according to @format as %vsprintf, except that the
 * string is allocated using #nih_alloc.
 *
 * If @parent is not %NULL, it should be a pointer to another allocated
 * block which will be used as the parent for this block.  When @parent
 * is freed, the returned string will be freed too.  If you have clean-up
 * that would need to be run, you can assign a destructor function using
 * the #nih_alloc_set_destructor function.
 *
 * Returns: newly allocated string or %NULL.
 **/
char *
nih_vsprintf (void       *parent,
	      const char *format,
	      va_list     args)
{
	size_t   len;
	va_list  args_copy;
	char    *str;

	va_copy (args_copy, args);

	len = vsnprintf (NULL, 0, format, args);

	str = nih_alloc (parent, len + 1);
	if (! str)
		return NULL;

	vsnprintf (str, len + 1, format, args_copy);

	return str;
}
