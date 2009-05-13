/* nih-dbus-tool
 *
 * method.c - method parsing and generation
 *
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>.
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

#include <string.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/list.h>
#include <nih/string.h>
#include <nih/logging.h>
#include <nih/error.h>

#include "symbol.h"
#include "interface.h"
#include "method.h"
#include "argument.h"
#include "parse.h"
#include "indent.h"
#include "type.h"
#include "marshal.h"
#include "demarshal.h"
#include "errors.h"


/**
 * method_name_valid:
 * @name: Member name to verify.
 *
 * Verifies whether @name matches the specification for a D-Bus interface
 * member name, and thus is valid for a method.
 *
 * Returns: TRUE if valid, FALSE if not.
 **/
int
method_name_valid (const char *name)
{
	nih_assert (name != NULL);

	/* We can get away with just using strlen() here even through name
	 * is in UTF-8 because all the valid characters are ASCII.
	 */
	for (size_t i = 0; i < strlen (name); i++) {
		/* Names may contain digits, but not at the beginning. */
		if ((name[i] >= '0') && (name[i] <= '9')) {
			if (i == 0)
				return FALSE;

			continue;
		}

		/* Valid characters anywhere are [A-Za-z_] */
		if (   ((name[i] < 'A') || (name[i] > 'Z'))
		    && ((name[i] < 'a') || (name[i] > 'z'))
		    && (name[i] != '_'))
			return FALSE;
	}

	/* Name must be at least 1 character and no more than 255 characters */
	if ((strlen (name) < 1) || (strlen (name) > 255))
		return FALSE;

	return TRUE;
}


/**
 * method_new:
 * @parent: parent object for new method,
 * @name: D-Bus name of method.
 *
 * Allocates a new D-Bus object Method data structure, with the D-Bus name
 * set to @name.  The returned structure is not placed into any list and
 * has no arguments.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned method.  When all parents
 * of the returned method are freed, the returned method will also be
 * freed.
 *
 * Returns: the new method or NULL if the allocation failed.
 **/
Method *
method_new (const void *parent,
	    const char *name)
{
	Method *method;

	nih_assert (name != NULL);

	method = nih_new (parent, Method);
	if (! method)
		return NULL;

	nih_list_init (&method->entry);

	nih_alloc_set_destructor (method, nih_list_destroy);

	method->name = nih_strdup (method, name);
	if (! method->name) {
		nih_free (method);
		return NULL;
	}

	method->symbol = NULL;
	method->async = FALSE;
	method->no_reply = FALSE;
	method->deprecated = FALSE;

	nih_list_init (&method->arguments);

	return method;
}


/**
 * method_start_tag:
 * @xmlp: XML parser,
 * @tag: name of XML tag being parsed,
 * @attr: NULL-terminated array of attribute name and value pairs.
 *
 * This function is called by parse_start_tag() for a "method"
 * start tag, a child of the "interface" tag that defines a method the
 * D-Bus interface specifies.
 *
 * If the method does not appear within an interface tag a warning is
 * emitted and the tag will be ignored.
 *
 * Methods must have a "name" attribute containing the D-Bus name of the
 * method.
 *
 * Any unknown attributes result in a warning and will be ignored.
 *
 * A Method object will be allocated and pushed onto the stack, this is
 * not added to the interface until the end tag is found.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
method_start_tag (XML_Parser    xmlp,
		  const char *  tag,
		  char * const *attr)
{
	ParseContext *context;
	ParseStack *  parent;
	Method *      method;
	char * const *key;
	char * const *value;
	const char *  name = NULL;

	nih_assert (xmlp != NULL);
	nih_assert (tag != NULL);
	nih_assert (attr != NULL);

	context = XML_GetUserData (xmlp);
	nih_assert (context != NULL);

	/* Methods should only appear inside interfaces. */
	parent = parse_stack_top (&context->stack);
	if ((! parent) || (parent->type != PARSE_INTERFACE)) {
		nih_warn ("%s:%zu:%zu: %s", context->filename,
			  (size_t)XML_GetCurrentLineNumber (xmlp),
			  (size_t)XML_GetCurrentColumnNumber (xmlp),
			  _("Ignored unexpected <method> tag"));

		if (! parse_stack_push (NULL, &context->stack,
					PARSE_IGNORED, NULL))
			nih_return_system_error (-1);

		return 0;
	}

	/* Retrieve the name from the attributes */
	for (key = attr; key && *key; key += 2) {
		value = key + 1;
		nih_assert (value && *value);

		if (! strcmp (*key, "name")) {
			name = *value;
		} else {
			nih_warn ("%s:%zu:%zu: %s: %s", context->filename,
				  (size_t)XML_GetCurrentLineNumber (xmlp),
				  (size_t)XML_GetCurrentColumnNumber (xmlp),
				  _("Ignored unknown <method> attribute"),
				  *key);
		}
	}

	/* Check we have a name and that it's valid */
	if (! name)
		nih_return_error (-1, METHOD_MISSING_NAME,
				  _(METHOD_MISSING_NAME_STR));
	if (! method_name_valid (name))
		nih_return_error (-1, METHOD_INVALID_NAME,
				  _(METHOD_INVALID_NAME_STR));

	/* Allocate a Method object and push onto the stack */
	method = method_new (NULL, name);
	if (! method)
		nih_return_system_error (-1);

	if (! parse_stack_push (NULL, &context->stack, PARSE_METHOD, method)) {
		nih_error_raise_system ();
		nih_free (method);
		return -1;
	}

	return 0;
}

/**
 * method_end_tag:
 * @xmlp: XML parser,
 * @tag: name of XML tag being parsed.
 *
 * This function is called by parse_end_tag() for a "method" end
 * tag, and matches a call to method_start_tag() made at the same parsing
 * level.
 *
 * The method is added to the list of methods defined by the parent
 * interface.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
method_end_tag (XML_Parser  xmlp,
		const char *tag)
{
	ParseContext *context;
	ParseStack *  entry;
	ParseStack *  parent;
	Method *      method;
	Method *      conflict;
	Interface *   interface;

	nih_assert (xmlp != NULL);
	nih_assert (tag != NULL);

	context = XML_GetUserData (xmlp);
	nih_assert (context != NULL);

	entry = parse_stack_top (&context->stack);
	nih_assert (entry != NULL);
	nih_assert (entry->type == PARSE_METHOD);
	method = entry->method;

	/* Generate a symbol from the name */
	if (! method->symbol) {
		method->symbol = symbol_from_name (method, method->name);
		if (! method->symbol)
			nih_return_no_memory_error (-1);
	}

	nih_list_remove (&entry->entry);
	parent = parse_stack_top (&context->stack);
	nih_assert (parent != NULL);
	nih_assert (parent->type == PARSE_INTERFACE);
	interface = parent->interface;

	/* Make sure there's not a conflict before adding the method */
	conflict = interface_lookup_method (interface, method->symbol);
	if (conflict) {
		nih_error_raise_printf (METHOD_DUPLICATE_SYMBOL,
					_(METHOD_DUPLICATE_SYMBOL_STR),
					method->symbol, conflict->name);
		return -1;
	}

	nih_debug ("Add %s method to %s interface",
		   method->name, interface->name);
	nih_list_add (&interface->methods, &method->entry);
	nih_ref (method, interface);

	nih_free (entry);

	return 0;
}


/**
 * method_annotation:
 * @method: method object annotation applies to,
 * @name: annotation name,
 * @value: annotation value.
 *
 * Handles applying the annotation @name with value @value to the method
 * @method.  Methods may be annotated as deprecated, that a client should
 * expect no reply, an alternate symbol name may be specified or that the
 * object implementation will be asynchronous.
 *
 * Unknown annotations or illegal values to the known annotations result
 * in an error being raised.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
method_annotation (Method *    method,
		   const char *name,
		   const char *value)
{
	nih_assert (method != NULL);
	nih_assert (name != NULL);
	nih_assert (value != NULL);

	if (! strcmp (name, "org.freedesktop.DBus.Deprecated")) {
		if (! strcmp (value, "true")) {
			nih_debug ("Marked %s method as deprecated",
				   method->name);
			method->deprecated = TRUE;
		} else if (! strcmp (value, "false")) {
			nih_debug ("Marked %s method as not deprecated",
				   method->name);
			method->deprecated = FALSE;
		} else {
			nih_return_error (-1, METHOD_ILLEGAL_DEPRECATED,
					  _(METHOD_ILLEGAL_DEPRECATED_STR));
		}

	} else if (! strcmp (name, "org.freedesktop.DBus.Method.NoReply")) {
		if (! strcmp (value, "true")) {
			nih_debug ("Marked %s method to expect no reply",
				   method->name);
			method->no_reply = TRUE;
		} else if (! strcmp (value, "false")) {
			nih_debug ("Marked %s method to expect a reply",
				   method->name);
			method->no_reply = FALSE;
		} else {
			nih_return_error (-1, METHOD_ILLEGAL_NO_REPLY,
					  _(METHOD_ILLEGAL_NO_REPLY_STR));
		}

	} else if (! strcmp (name, "com.netsplit.Nih.Symbol")) {
		if (symbol_valid (value)) {
			if (method->symbol)
				nih_unref (method->symbol, method);

			method->symbol = nih_strdup (method, value);
			if (! method->symbol)
				nih_return_no_memory_error (-1);

			nih_debug ("Set %s method symbol to %s",
				   method->name, method->symbol);
		} else {
			nih_return_error (-1, METHOD_INVALID_SYMBOL,
					  _(METHOD_INVALID_SYMBOL_STR));
		}

	} else if (! strcmp (name, "com.netsplit.Nih.Method.Async")) {
		if (! strcmp (value, "true")) {
			nih_debug ("Marked %s method as async",
				   method->name);
			method->async = TRUE;
		} else if (! strcmp (value, "false")) {
			nih_debug ("Marked %s method as non-async",
				   method->name);
			method->async = FALSE;
		} else {
			nih_return_error (-1, METHOD_ILLEGAL_ASYNC,
					  _(METHOD_ILLEGAL_ASYNC_STR));
		}

	} else {
		nih_error_raise_printf (METHOD_UNKNOWN_ANNOTATION,
					"%s: %s: %s",
					_(METHOD_UNKNOWN_ANNOTATION_STR),
					method->name, name);
		return -1;
	}

	return 0;
}


/**
 * method_lookup_argument:
 * @method: method to search,
 * @symbol: argument symbol to find.
 *
 * Finds a argument in @method's arguments list which has the generated
 * or supplied C symbol @symbol.
 *
 * Returns: argument found or NULL if no argument matches.
 **/
Argument *
method_lookup_argument (Method *    method,
			const char *symbol)
{
	nih_assert (method != NULL);
	nih_assert (symbol != NULL);

	NIH_LIST_FOREACH (&method->arguments, iter) {
		Argument *argument = (Argument *)iter;

		if (argument->symbol
		    && (! strcmp (argument->symbol, symbol)))
			return argument;
	}

	return NULL;
}


/**
 * method_object_function:
 * @parent: parent object for new string.
 * @method: method to generate function for,
 * @name: name of function to generate,
 * @handler_name: name of handler function to call,
 * @prototypes: list to append function prototypes to,
 * @externs: list to append definitions of extern function prototypes to.
 *
 * Generates C code for a function @name to handle the method @method,
 * demarshalling the incoming arguments, calling a function named
 * @handler_name and marshalling the output arguments into a reply or
 * responding with an error.
 *
 * The prototype of the function is given as a TypeFunc object appended to
 * the @prototypes list, with the name as @name itself.  Should the C code
 * call other functions that need to be defined, similar TypeFunc objects
 * will be appended to the @externs list.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned string.  When all parents
 * of the returned string are freed, the return string will also be
 * freed.
 *
 * Returns: newly allocated string or NULL if insufficient memory.
 **/
char *
method_object_function (const void *parent,
			Method *    method,
			const char *name,
			const char *handler_name,
			NihList *   prototypes,
			NihList *   externs)
{
	NihList             locals;
	nih_local TypeFunc *func = NULL;
	TypeVar *           arg;
	nih_local TypeVar * iter_var = NULL;
	nih_local TypeVar * reply_var = NULL;
	nih_local char *    demarshal_block = NULL;
	nih_local char *    call_block = NULL;
	nih_local TypeFunc *handler_func = NULL;
	NihListEntry *      attrib;
	nih_local char *    marshal_block = NULL;
	nih_local char *    vars_block = NULL;
	nih_local char *    body = NULL;
	char *              code = NULL;

	nih_assert (method != NULL);
	nih_assert (name != NULL);
	nih_assert (handler_name != NULL);
	nih_assert (prototypes != NULL);
	nih_assert (externs != NULL);

	nih_list_init (&locals);

	/* The function returns a DBusHandlerResult since it's a handling
	 * function, and accepts arguments for the object and message.
	 * We don't have any attributes, not even "deprecated" for a
	 * deprecated method since we always want to implement it without
	 * error.
	 */
	func = type_func_new (NULL, "DBusHandlerResult", name);
	if (! func)
		return NULL;

	arg = type_var_new (func, "NihDBusObject *", "object");
	if (! arg)
		return NULL;

	nih_list_add (&func->args, &arg->entry);

	arg = type_var_new (func, "NihDBusMessage *", "message");
	if (! arg)
		return NULL;

	nih_list_add (&func->args, &arg->entry);

	/* The function requires a local iterator for the message, and a
	 * reply message pointer.  Rather than deal with these by hand,
	 * it's far easier to put them on the locals list and deal with
	 * them along with the rest.
	 */
	iter_var = type_var_new (NULL, "DBusMessageIter", "iter");
	if (! iter_var)
		return NULL;

	nih_list_add (&locals, &iter_var->entry);

	reply_var = type_var_new (NULL, "DBusMessage *", "reply");
	if (! reply_var)
		return NULL;

	nih_list_add (&locals, &reply_var->entry);


	/* Begin the pre-handler demarshalling block with the iterator */
	if (! nih_strcat_sprintf (&demarshal_block, NULL,
				  "/* Iterate the arguments to the message and demarshal into arguments\n"
				  " * for our own function call.\n"
				  " */\n"
				  "dbus_message_iter_init (message->message, &iter);\n"
				  "\n"))
		return NULL;

	/* Begin the handler calling block.  The handler function always
	 * has a warn_unusued_result attribute, just for completeness.
	 */
	if (! nih_strcat_sprintf (&call_block, NULL,
				  "/* Call the handler function */\n"
				  "if (%s (object->data, message",
				  handler_name))
		return NULL;

	handler_func = type_func_new (NULL, "int", handler_name);
	if (! handler_func)
		return NULL;

	attrib = nih_list_entry_new (handler_func);
	if (! attrib)
		return NULL;

	attrib->str = nih_strdup (attrib, "warn_unused_result");
	if (! attrib->str)
		return NULL;

	nih_list_add (&handler_func->attribs, &attrib->entry);

	arg = type_var_new (handler_func, "void *", "data");
	if (! arg)
		return NULL;

	nih_list_add (&handler_func->args, &arg->entry);

	arg = type_var_new (handler_func, "NihDBusMessage *", "message");
	if (! arg)
		return NULL;

	nih_list_add (&handler_func->args, &arg->entry);

	/* Begin the post-handler marshalling block with the creation of
	 * the return message and re-using the iterator to marshal it.
	 */
	if (! method->async)
		if (! nih_strcat_sprintf (&marshal_block, NULL,
					  "/* Construct the reply message. */\n"
					  "reply = dbus_message_new_method_return (message->message);\n"
					  "if (! reply)\n"
					  "\tcontinue;\n"
					  "\n"
					  "dbus_message_iter_init_append (reply, &iter);\n"))
			return NULL;

	/* Iterate over the method arguments, for each input argument we
	 * append the code to the pre-handler demarshalling code and for
	 * each output argument we append the code to the post-handler
	 * marshalling code.  At the same time, we build up the handler
	 * call itself and transfer the actual arguments to the locals
	 * list.
	 */
	NIH_LIST_FOREACH (&method->arguments, iter) {
		Argument *        argument = (Argument *)iter;
		NihList           arg_vars;
		NihList           arg_locals;
		DBusSignatureIter iter;
		nih_local char *  oom_error_code = NULL;
		nih_local char *  type_error_code = NULL;
		nih_local char *  block = NULL;

		nih_list_init (&arg_vars);
		nih_list_init (&arg_locals);

		dbus_signature_iter_init (&iter, argument->type);

		switch (argument->direction) {
		case NIH_DBUS_ARG_IN:
			/* In case of out of memory, let D-Bus decide what to
			 * do.  In case of type error, we return an error to
			 * D-Bus.
			 */
			oom_error_code = nih_strdup (NULL,
						     "return DBUS_HANDLER_RESULT_NEED_MEMORY;\n");
			if (! oom_error_code)
				return NULL;

			type_error_code = nih_sprintf (NULL,
						       "reply = dbus_message_new_error (message->message, DBUS_ERROR_INVALID_ARGS,\n"
						       "                                _(\"Invalid arguments to %s method\"));\n"
						       "if (! reply)\n"
						       "\treturn DBUS_HANDLER_RESULT_NEED_MEMORY;\n"
						       "\n"
						       "if (! dbus_connection_send (message->conn, reply, NULL)) {\n"
						       "\tdbus_message_unref (reply);\n"
						       "\treturn DBUS_HANDLER_RESULT_NEED_MEMORY;\n"
						       "}\n"
						       "\n"
						       "dbus_message_unref (reply);\n"
						       "return DBUS_HANDLER_RESULT_HANDLED;\n",
						       method->name);
			if (! type_error_code)
				return NULL;

			block = demarshal (NULL, &iter, "message", "iter",
					   argument->symbol,
					   oom_error_code,
					   type_error_code,
					   &arg_vars, &arg_locals);
			if (! block)
				return NULL;

			if (! nih_strcat_sprintf (&demarshal_block, NULL,
						  "%s"
						  "\n",
						  block))
				return NULL;

			NIH_LIST_FOREACH_SAFE (&arg_vars, iter) {
				TypeVar *var = (TypeVar *)iter;

				if (! nih_strcat_sprintf (&call_block, NULL,
							  ", %s",
							  var->name))
					return NULL;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, call_block);

				/* Handler argument is const */
				arg = type_var_new (handler_func, var->type,
						    var->name);
				if (! arg)
					return NULL;

				if (! type_to_const (&arg->type, arg))
					return NULL;

				nih_list_add (&handler_func->args,
					      &arg->entry);
			}

			NIH_LIST_FOREACH_SAFE (&arg_locals, iter) {
				TypeVar *var = (TypeVar *)iter;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, call_block);
			}

			break;
		case NIH_DBUS_ARG_OUT:
			/* Asynchronous methods don't have output arguments */
			if (method->async)
				continue;

			/* In case of out of memory, we can't just return
			 * beacuse handler side-effects have already happened.
			 * Discard the message and loop again to try and
			 * reconstruct it.
			 */
			oom_error_code = nih_strdup (NULL,
						     "dbus_message_unref (reply);\n"
						     "reply = NULL;\n"
						     "continue;\n");
			if (! oom_error_code)
				return NULL;

			block = marshal (NULL, &iter, "iter", argument->symbol,
					 oom_error_code,
					 &arg_vars, &arg_locals);
			if (! block)
				return NULL;

			if (! nih_strcat_sprintf (&marshal_block, NULL,
						  "\n"
						  "%s",
						  block))
				return NULL;

			/* Need to pass the address of the return variable */
			NIH_LIST_FOREACH_SAFE (&arg_vars, iter) {
				TypeVar *var = (TypeVar *)iter;

				if (! nih_strcat_sprintf (&call_block, NULL,
							  ", &%s",
							  var->name))
					return NULL;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, call_block);

				/* Handler argument is a pointer */
				arg = type_var_new (handler_func, var->type,
						    var->name);
				if (! arg)
					return NULL;

				if (! type_to_pointer (&arg->type, arg))
					return NULL;

				nih_list_add (&handler_func->args,
					      &arg->entry);
			}

			NIH_LIST_FOREACH_SAFE (&arg_locals, iter) {
				TypeVar *var = (TypeVar *)iter;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, call_block);
			}

			break;
		default:
			nih_assert_not_reached ();
		}
	}

	/* Complete the demarshalling block, checking for any unexpected
	 * arguments which we also want to error on.
	 */
	if (! nih_strcat_sprintf (&demarshal_block, NULL,
				  "if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INVALID) {\n"
				  "\treply = dbus_message_new_error (message->message, DBUS_ERROR_INVALID_ARGS,\n"
				  "\t                                _(\"Invalid arguments to %s method\"));\n"
				  "\tif (! reply)\n"
				  "\t\treturn DBUS_HANDLER_RESULT_NEED_MEMORY;\n"
				  "\n"
				  "\tif (! dbus_connection_send (message->conn, reply, NULL)) {\n"
				  "\t\tdbus_message_unref (reply);\n"
				  "\t\treturn DBUS_HANDLER_RESULT_NEED_MEMORY;\n"
				  "\t}\n"
				  "\n"
				  "\tdbus_message_unref (reply);\n"
				  "\treturn DBUS_HANDLER_RESULT_HANDLED;\n"
				  "}\n"
				  "\n",
				  method->name))
	    return NULL;

	/* Complete the call block, handling errors from the called function;
	 * out of memory is easy, we return that to D-Bus and let that decide
	 * what to do.  Other errors must be returned, even if we run out of
	 * memory while trying to return them because side-effects of the
	 * handler function may have already happened.
	 */
	if (! nih_strcat_sprintf (&call_block, NULL, ") < 0) {\n"
				  "\tNihError *err;\n"
				  "\n"
				  "\terr = nih_error_get ();\n"
				  "\tif (err->number == ENOMEM) {\n"
				  "\t\tnih_free (err);\n"
				  "\n"
				  "\t\treturn DBUS_HANDLER_RESULT_NEED_MEMORY;\n"
				  "\t} else if (err->number == NIH_DBUS_ERROR) {\n"
				  "\t\tNihDBusError *dbus_err = (NihDBusError *)err;\n"
				  "\n"
				  "\t\treply = NIH_MUST (dbus_message_new_error (message->message, dbus_err->name, err->message));\n"
				  "\t\tnih_free (err);\n"
				  "\n"
				  "\t\tNIH_MUST (dbus_connection_send (message->conn, reply, NULL));\n"
				  "\n"
				  "\t\tdbus_message_unref (reply);\n"
				  "\t\treturn DBUS_HANDLER_RESULT_HANDLED;\n"
				  "\t} else {\n"
				  "\t\treply = NIH_MUST (dbus_message_new_error (message->message, DBUS_ERROR_FAILED, err->message));\n"
				  "\t\tnih_free (err);\n"
				  "\n"
				  "\t\tNIH_MUST (dbus_connection_send (message->conn, reply, NULL));\n"
				  "\n"
				  "\t\tdbus_message_unref (reply);\n"
				  "\t\treturn DBUS_HANDLER_RESULT_HANDLED;\n"
				  "\t}\n"
				  "}\n"
				  "\n"))
		return NULL;

	if (! method->async)
		if (! nih_strcat_sprintf (&call_block, NULL,
					  "/* If the sender doesn't care about a reply, don't bother wasting\n"
					  " * effort constructing and sending one.\n"
					  " */\n"
					  "if (dbus_message_get_no_reply (message->message))\n"
					  "\treturn DBUS_HANDLER_RESULT_HANDLED;\n"
					  "\n"))
			return NULL;

	/* Indent the marshalling block, it goes inside a while loop */
	if (! method->async)
		if (! indent (&marshal_block, NULL, 1))
			return NULL;

	/* Lay out the function body, indenting it all before placing it
	 * in the function code.
	 */
	vars_block = type_var_layout (NULL, &locals);
	if (! vars_block)
		return NULL;

	if (! nih_strcat_sprintf (&body, NULL,
				  "%s"
				  "\n"
				  "nih_assert (object != NULL);\n"
				  "nih_assert (message != NULL);\n"
				  "\n"
				  "%s"
				  "%s",
				  vars_block,
				  demarshal_block,
				  call_block))
		return NULL;

	if (! method->async) {
		if (! nih_strcat_sprintf (&body, NULL,
					  "do {\n"
					  "%s"
					  "} while (! reply);\n"
					  "\n"
					  "/* Send the reply, appending it to the outgoing queue. */\n"
					  "NIH_MUST (dbus_connection_send (message->conn, reply, NULL));\n"
					  "\n"
					  "dbus_message_unref (reply);\n"
					  "\n",
					  marshal_block))
			return NULL;
	}

	if (! nih_strcat_sprintf (&body, NULL,
				  "return DBUS_HANDLER_RESULT_HANDLED;\n"))
		return NULL;

	if (! indent (&body, NULL, 1))
		return NULL;

	/* Function header */
	code = type_func_to_string (parent, func);
	if (! code)
		return NULL;

	if (! nih_strcat_sprintf (&code, parent,
				  "{\n"
				  "%s"
				  "}\n",
				  body)) {
		nih_free (code);
		return NULL;
	}

	/* Append the functions to the prototypes and externs lists */
	nih_list_add (prototypes, &func->entry);
	nih_ref (func, code);

	nih_list_add (externs, &handler_func->entry);
	nih_ref (handler_func, code);

	return code;
}

/**
 * method_reply_function:
 * @parent: parent object for new string.
 * @method: method to generate function for,
 * @name: name of function to generate,
 * @prototypes: list to append function prototypes to,
 * @externs: list to append definitions of extern function prototypes to.
 *
 * Generates C code for a function @name to send a reply for the method
 * @method by marshalling the arguments.
 *
 * The prototype of the function is given as a TypeFunc object appended to
 * the @prototypes list, with the name as @name itself.  Should the C code
 * call other functions that need to be defined, similar TypeFunc objects
 * will be appended to the @externs list.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned string.  When all parents
 * of the returned string are freed, the return string will also be
 * freed.
 *
 * Returns: newly allocated string or NULL if insufficient memory.
 **/
char *
method_reply_function (const void *parent,
		       Method *    method,
		       const char *name,
		       NihList *   prototypes,
		       NihList *   externs)
{
	NihList             locals;
	nih_local TypeFunc *func = NULL;
	TypeVar *           arg;
	NihListEntry *      attrib;
	nih_local TypeVar * reply_var = NULL;
	nih_local TypeVar * iter_var = NULL;
	nih_local char *    marshal_block = NULL;
	nih_local char *    assert_block = NULL;
	nih_local char *    vars_block = NULL;
	nih_local char *    body = NULL;
	char *              code = NULL;

	nih_assert (method != NULL);
	nih_assert (name != NULL);
	nih_assert (prototypes != NULL);
	nih_assert (externs != NULL);

	nih_list_init (&locals);

	/* The function returns an integer, and accepts an argument for
	 * the original message.  The integer indicates whether an error
	 * occurred, so we want if the result isn't used; but like the
	 * method handler, we don't care about marshalling.
	 */
	func = type_func_new (NULL, "int", name);
	if (! func)
		return NULL;

	arg = type_var_new (func, "NihDBusMessage *", "message");
	if (! arg)
		return NULL;

	nih_list_add (&func->args, &arg->entry);

	attrib = nih_list_entry_new (func);
	if (! attrib)
		return NULL;

	attrib->str = nih_strdup (attrib, "warn_unused_result");
	if (! attrib->str)
		return NULL;

	nih_list_add (&func->attribs, &attrib->entry);

	/* The function requires a reply message pointer, which we allocate,
	 * and an iterator for it to append the arguments.  Rather than
	 * deal with these by hand, it's far easier to put them on the
	 * locals list and deal with them along with the rest.
	 */
	reply_var = type_var_new (NULL, "DBusMessage *", "reply");
	if (! reply_var)
		return NULL;

	nih_list_add (&locals, &reply_var->entry);

	iter_var = type_var_new (NULL, "DBusMessageIter", "iter");
	if (! iter_var)
		return NULL;

	nih_list_add (&locals, &iter_var->entry);


	/* Create the reply and set up the iterator to append to it. */
	if (! nih_strcat_sprintf (&marshal_block, NULL,
				  "/* If the sender doesn't care about a reply, don't bother wasting\n"
				  " * effort constructing and sending one.\n"
				  " */\n"
				  "if (dbus_message_get_no_reply (message->message))\n"
				  "\treturn 0;\n"
				  "\n"
				  "/* Construct the reply message. */\n"
				  "reply = dbus_message_new_method_return (message->message);\n"
				  "if (! reply)\n"
				  "\treturn -1;\n"
				  "\n"
				  "dbus_message_iter_init_append (reply, &iter);\n"
				  "\n"))
		return NULL;

	/* Iterate over the method's output arguments, for each one we
	 * append the code to the marshalling code and at the same time
	 * build up our own expected arguments themselves.
	 */
	NIH_LIST_FOREACH (&method->arguments, iter) {
		Argument *        argument = (Argument *)iter;
		NihList           arg_vars;
		NihList           arg_locals;
		DBusSignatureIter iter;
		nih_local char *  oom_error_code = NULL;
		nih_local char *  type_error_code = NULL;
		nih_local char *  block = NULL;

		if (argument->direction != NIH_DBUS_ARG_OUT)
			continue;

		nih_list_init (&arg_vars);
		nih_list_init (&arg_locals);

		dbus_signature_iter_init (&iter, argument->type);

		/* In case of out of memory, simply return; the caller
		 * can try again.
		 */
		oom_error_code = nih_strdup (NULL,
					     "dbus_message_unref (reply);\n"
					     "return -1;\n");
		if (! oom_error_code)
			return NULL;

		block = marshal (NULL, &iter, "iter", argument->symbol,
				 oom_error_code,
				 &arg_vars, &arg_locals);
		if (! block)
			return NULL;

		if (! nih_strcat_sprintf (&marshal_block, NULL,
					  "%s"
					  "\n",
					  block))
			return NULL;

		/* We take a parameter of the expected type and name of
		 * the marshal input variable; if it's a pointer, we
		 * assert that it's not NULL and make sure it's const.
		 */
		NIH_LIST_FOREACH_SAFE (&arg_vars, iter) {
			TypeVar *var = (TypeVar *)iter;

			if (! type_to_const (&var->type, var))
				return NULL;

			if (strchr (var->type, '*'))
				if (! nih_strcat_sprintf (&assert_block, NULL,
							  "nih_assert (%s != NULL);\n",
							  var->name))
					return NULL;

			nih_list_add (&func->args, &var->entry);
			nih_ref (var, func);
		}

		NIH_LIST_FOREACH_SAFE (&arg_locals, iter) {
			TypeVar *var = (TypeVar *)iter;

			nih_list_add (&locals, &var->entry);
			nih_ref (var, marshal_block);
		}
	}

	/* Lay out the function body, indenting it all before placing it
	 * in the function code.
	 */
	vars_block = type_var_layout (NULL, &locals);
	if (! vars_block)
		return NULL;

	if (! nih_strcat_sprintf (&body, NULL,
				  "%s"
				  "\n"
				  "nih_assert (message != NULL);\n"
				  "%s"
				  "\n"
				  "%s"
				  "/* Send the reply, appending it to the outgoing queue. */\n"
				  "if (! dbus_connection_send (message->conn, reply, NULL)) {\n"
				  "\tdbus_message_unref (reply);\n"
				  "\treturn -1;\n"
				  "}\n"
				  "\n"
				  "dbus_message_unref (reply);\n"
				  "\n"
				  "return 0;\n",
				  vars_block,
				  assert_block,
				  marshal_block))
		return NULL;

	if (! indent (&body, NULL, 1))
		return NULL;

	/* Function header */
	code = type_func_to_string (parent, func);
	if (! code)
		return NULL;

	if (! nih_strcat_sprintf (&code, parent,
				  "{\n"
				  "%s"
				  "}\n",
				  body)) {
		nih_free (code);
		return NULL;
	}

	/* Append the functions to the prototypes and externs lists */
	nih_list_add (prototypes, &func->entry);
	nih_ref (func, code);

	return code;
}


/**
 * method_proxy_sync_function:
 * @parent: parent object for new string.
 * @interface_name: name of interface,
 * @method: method to generate function for,
 * @name: name of function to generate,
 * @prototypes: list to append function prototypes to,
 * @externs: list to append definitions of extern function prototypes to.
 *
 * Generates C code for a function @name to make a synchronous method
 * call for the method @method by marshalling the arguments.  The interface
 * name of the method must be supplied in @interface_name.
 *
 * The prototype of the function is given as a TypeFunc object appended to
 * the @prototypes list, with the name as @name itself.  Should the C code
 * call other functions that need to be defined, similar TypeFunc objects
 * will be appended to the @externs list.
 *
 * If @parent is not NULL, it should be a pointer to another object which
 * will be used as a parent for the returned string.  When all parents
 * of the returned string are freed, the return string will also be
 * freed.
 *
 * Returns: newly allocated string or NULL if insufficient memory.
 **/
char *
method_proxy_sync_function (const void *parent,
			    const char *interface_name,
			    Method *    method,
			    const char *name,
			    NihList *   prototypes,
			    NihList *   externs)
{
	NihList             locals;
	nih_local TypeFunc *func = NULL;
	TypeVar *           arg;
	NihListEntry *      attrib;
	nih_local TypeVar * message_var = NULL;
	nih_local TypeVar * iter_var = NULL;
	nih_local TypeVar * error_var = NULL;
	nih_local TypeVar * reply_var = NULL;
	nih_local TypeVar * parent_var = NULL;
	nih_local char *    marshal_block = NULL;
	nih_local char *    demarshal_block = NULL;
	nih_local char *    assert_block = NULL;
	nih_local char *    vars_block = NULL;
	nih_local char *    body = NULL;
	char *              code = NULL;

	nih_assert (interface_name != NULL);
	nih_assert (method != NULL);
	nih_assert (name != NULL);
	nih_assert (prototypes != NULL);
	nih_assert (externs != NULL);

	nih_list_init (&locals);

	/* The function returns a message context, and takes the proxy object
	 * as the argument along with the input and output arguments of
	 * the method call.  The integer indicates whether an error
	 * occurred, so we want warning if the result isn't used.  Since
	 * this is used by the client, we also add a deprecated attribute
	 * if the method is deprecated.
	 */
	func = type_func_new (NULL, "NihDBusMessage *", name);
	if (! func)
		return NULL;

	attrib = nih_list_entry_new (func);
	if (! attrib)
		return NULL;

	attrib->str = nih_strdup (attrib, "warn_unused_result");
	if (! attrib->str)
		return NULL;

	nih_list_add (&func->attribs, &attrib->entry);

	attrib = nih_list_entry_new (func);
	if (! attrib)
		return NULL;

	attrib->str = nih_strdup (attrib, "malloc");
	if (! attrib->str)
		return NULL;

	nih_list_add (&func->attribs, &attrib->entry);

	if (method->deprecated) {
		attrib = nih_list_entry_new (func);
		if (! attrib)
			return NULL;

		attrib->str = nih_strdup (attrib, "deprecated");
		if (! attrib->str)
			return NULL;

		nih_list_add (&func->attribs, &attrib->entry);
	}

	arg = type_var_new (func, "NihDBusProxy *", "proxy");
	if (! arg)
		return NULL;

	nih_list_add (&func->args, &arg->entry);

	assert_block = nih_strcat (&assert_block, NULL,
				   "nih_assert (proxy != NULL);\n");
	if (! assert_block)
		return NULL;


	/* The function requires a message pointer, which we allocate,
	 * and an iterator for it to append the arguments.  We also need
	 * a reply message pointer as well, an encapsulating (parent)
	 * object and an error object.  Rather than deal with these by hand,
	 * it's far easier to put them on the locals list and deal with them
	 * along with the rest.
	 */
	message_var = type_var_new (NULL, "DBusMessage *", "message");
	if (! message_var)
		return NULL;

	nih_list_add (&locals, &message_var->entry);

	iter_var = type_var_new (NULL, "DBusMessageIter", "iter");
	if (! iter_var)
		return NULL;

	nih_list_add (&locals, &iter_var->entry);

	error_var = type_var_new (NULL, "DBusError", "error");
	if (! error_var)
		return NULL;

	nih_list_add (&locals, &error_var->entry);

	reply_var = type_var_new (NULL, "DBusMessage *", "reply");
	if (! reply_var)
		return NULL;

	nih_list_add (&locals, &reply_var->entry);

	parent_var = type_var_new (NULL, "NihDBusMessage *", "msg");
	if (! parent_var)
		return NULL;

	nih_list_add (&locals, &parent_var->entry);


	/* Create the message and set up the iterator to append to it.
	 * When demarshalling we set up the iterator to go over the reply.
	 */
	if (! nih_strcat_sprintf (&marshal_block, NULL,
				  "/* Construct the method call message. */\n"
				  "message = dbus_message_new_method_call (proxy->name, proxy->path, \"%s\", \"%s\");\n"
				  "if (! message)\n"
				  "\tnih_return_no_memory_error (NULL);\n"
				  "\n"
				  "dbus_message_iter_init_append (message, &iter);\n"
				  "\n",
				  interface_name, method->name))
		return NULL;

	/* FIXME autostart? */

	if (! nih_strcat_sprintf (&demarshal_block, NULL,
				  "dbus_message_unref (message);\n"
				  "\n"
				  "/* Create a message context for the reply, then iterate\n"
				  " * its arguments.  This contexts holds a reference, so\n"
				  " * we can drop the one we've already received.\n"
				  " */\n"
				  "msg = NIH_MUST (nih_dbus_message_new (proxy, proxy->conn, reply));\n"
				  "dbus_message_unref (reply);\n"
				  "dbus_message_iter_init (msg->message, &iter);\n"
				  "\n"))
		return NULL;

	/* Iterate over the method arguments, for each input argument we
	 * append the code to the pre-call marshalling code and for
	 * each output argument we append the code to the post-call
	 * demarshalling code.
	 */
	NIH_LIST_FOREACH (&method->arguments, iter) {
		Argument *        argument = (Argument *)iter;
		NihList           arg_vars;
		NihList           arg_locals;
		DBusSignatureIter iter;
		nih_local char *  local_name = NULL;
		nih_local char *  oom_error_code = NULL;
		nih_local char *  type_error_code = NULL;
		nih_local char *  block = NULL;

		nih_list_init (&arg_vars);
		nih_list_init (&arg_locals);

		dbus_signature_iter_init (&iter, argument->type);

		switch (argument->direction) {
		case NIH_DBUS_ARG_IN:
			/* In case of out of memory, simply return; the caller
			 * can try again.
			 */
			oom_error_code = nih_strdup (NULL,
						     "nih_free (msg);\n"
						     "nih_return_no_memory_error (NULL);\n");
			if (! oom_error_code)
				return NULL;

			block = marshal (NULL, &iter, "iter", argument->symbol,
					 oom_error_code,
					 &arg_vars, &arg_locals);
			if (! block)
				return NULL;

			if (! nih_strcat_sprintf (&marshal_block, NULL,
						  "%s"
						  "\n",
						  block))
				return NULL;

			/* We take a parameter of the expected type and name of
			 * the marshal input variable; if it's a pointer, we
			 * assert that it's not NULL and make sure it's const.
			 */
			NIH_LIST_FOREACH_SAFE (&arg_vars, iter) {
				TypeVar *var = (TypeVar *)iter;

				if (! type_to_const (&var->type, var))
					return NULL;

				if (strchr (var->type, '*'))
					if (! nih_strcat_sprintf (&assert_block, NULL,
								  "nih_assert (%s != NULL);\n",
								  var->name))
						return NULL;

				nih_list_add (&func->args, &var->entry);
				nih_ref (var, func);
			}

			NIH_LIST_FOREACH_SAFE (&arg_locals, iter) {
				TypeVar *var = (TypeVar *)iter;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, marshal_block);
			}

			break;
		case NIH_DBUS_ARG_OUT:
			/* We can't write directly to the pointer argument
			 * we were given, instead we use a local variable
			 * and write out later.
			 */
			local_name = nih_sprintf (NULL, "%s_local",
						  argument->symbol);
			if (! local_name)
				return NULL;

			/* In case of out of memory, we can't just return
			 * because we've already made the method call so
			 * we loop over the code instead. But in case of
			 * type error in the returned arguments, all we
			 * can do is return an error.
			 */
			oom_error_code = nih_sprintf (NULL,
						      "*%s = NULL;\n"
						      "goto enomem;\n",
						      argument->symbol);
			if (! oom_error_code)
				return NULL;

			type_error_code = nih_strdup (NULL,
						      "nih_free (msg);\n"
						      "nih_return_error (NULL, NIH_DBUS_INVALID_ARGS,\n"
						      "                  _(NIH_DBUS_INVALID_ARGS_STR));\n");
			if (! type_error_code)
				return NULL;

			block = demarshal (NULL, &iter, "msg", "iter",
					   local_name,
					   oom_error_code,
					   type_error_code,
					   &arg_vars, &arg_locals);
			if (! block)
				return NULL;

			if (! nih_strcat (&block, NULL, "\n"))
				return NULL;

			/* We take a parameter as a pointer to the expected
			 * type and name of the demarshal output variable,
			 * asserting that it's not NULL.  We actually
			 * demarshal to a local variable though, to avoid
			 * dealing with that extra level of pointers.
,			 */
			NIH_LIST_FOREACH_SAFE (&arg_vars, iter) {
				TypeVar *       var = (TypeVar *)iter;
				nih_local char *arg_type = NULL;
				const char *    suffix;
				nih_local char *arg_name = NULL;
				TypeVar *       arg;

				/* Output variable */
				arg_type = nih_strdup (NULL, var->type);
				if (! arg_type)
					return NULL;

				if (! type_to_pointer (&arg_type, NULL))
					return NULL;

				nih_assert (! strncmp (var->name, local_name,
						       strlen (local_name)));
				suffix = var->name + strlen (local_name);

				arg_name = nih_sprintf (NULL, "%s%s",
							argument->symbol,
							suffix);
				if (! arg_name)
					return NULL;

				arg = type_var_new (func, arg_type, arg_name);
				if (! arg)
					return NULL;

				nih_list_add (&func->args, &arg->entry);

				if (! nih_strcat_sprintf (&assert_block, NULL,
							  "nih_assert (%s != NULL);\n",
							  arg->name))
					return NULL;

				/* Copy from local variable to output */
				if (! nih_strcat_sprintf (&block, NULL,
							  "*%s = %s;\n",
							  arg->name, var->name))
					return NULL;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, demarshal_block);
			}

			NIH_LIST_FOREACH_SAFE (&arg_locals, iter) {
				TypeVar *var = (TypeVar *)iter;

				nih_list_add (&locals, &var->entry);
				nih_ref (var, demarshal_block);
			}

			if (! indent (&block, NULL, 1))
				return NULL;

			if (! nih_strcat_sprintf (&demarshal_block, NULL,
						  "do {\n"
						  "\t__label__ enomem;\n"
						  "\n"
						  "%s"
						  "enomem: __attribute__ ((unused));\n"
						  "} while (! *%s);\n"
						  "\n",
						  block,
						  argument->symbol))
				return NULL;

			break;
		default:
			nih_assert_not_reached ();
		}
	}

	/* Complete the marshalling block by sending the message and checking
	 * for error replies.
	 */
	if (! nih_strcat_sprintf (&marshal_block, NULL,
				  "/* Send the message, and wait for the reply. */\n"
				  "dbus_error_init (&error);\n"
				  "\n"
				  "reply = dbus_connection_send_with_reply_and_block (proxy->conn, message, -1, &error);\n"
				  "if (! reply) {\n"
				  "\tdbus_message_unref (message);\n"
				  "\n"
				  "\tif (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY)) {\n"
				  "\t\tnih_error_raise_no_memory ();\n"
				  "\t} else {\n"
				  "\t\tnih_dbus_error_raise (error.name, error.message);\n"
				  "\t}\n"
				  "\n"
				  "\tdbus_error_free (&error);\n"
				  "\treturn NULL;\n"
				  "}\n"
				  "\n"))
		return NULL;

	/* Complete the demarshalling block, checking for any unexpected
	 * reply arguments which we also want to error on.
	 */
	if (! nih_strcat_sprintf (&demarshal_block, NULL,
				  "if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_INVALID) {\n"
				  "\tnih_free (msg);\n"
				  "\tnih_return_error (NULL, NIH_DBUS_INVALID_ARGS,\n"
				  "\t                  _(NIH_DBUS_INVALID_ARGS_STR));\n"
				  "}\n"))
		return NULL;

	/* Lay out the function body, indenting it all before placing it
	 * in the function code.
	 */
	vars_block = type_var_layout (NULL, &locals);
	if (! vars_block)
		return NULL;

	if (! nih_strcat_sprintf (&body, NULL,
				  "%s"
				  "\n"
				  "%s"
				  "\n"
				  "%s"
				  "%s"
				  "\n"
				  "return msg;\n",
				  vars_block,
				  assert_block,
				  marshal_block,
				  demarshal_block))
		return NULL;

	if (! indent (&body, NULL, 1))
		return NULL;

	/* Function header */
	code = type_func_to_string (parent, func);
	if (! code)
		return NULL;

	if (! nih_strcat_sprintf (&code, parent,
				  "{\n"
				  "%s"
				  "}\n",
				  body)) {
		nih_free (code);
		return NULL;
	}

	/* Append the function to the prototypes list */
	nih_list_add (prototypes, &func->entry);
	nih_ref (func, code);

	return code;
}
