/**
 * loudbus.c
 *   A D-Bus Client for Racket.
 *
 * Copyright (c) 2012-15 Zarni Htet, Alexandra Greenberg, Mark Lewis, 
 * Evan Manuella, Samuel A. Rebelsky, Hart Russell, Mani Tiwaree,
 * and Christine Tran.  All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


// +-------+----------------------------------------------------------
// | Notes |
// +-------+

/*
 
* Since Racket has moved exclusively to 3M-style garbage collection
  (or so it seems, since it no longer ships with the CGC libraries,
  this needs to be annotated for 3M, either manually or automatically.
  It's hard to debug automatically annotated code, so we've done our
  best to annotate it manually.

* This implementation is incomplete.  We expect to add other methods
  (and support for other types) in the future.

 */

// +---------+--------------------------------------------------------
// | Headers |
// +---------+

#include <stdlib.h>     // For malloc, random and such
#include <stdio.h>      // We use fprintf for error messages during
                        // development.
#include <time.h>       // For seeing our random number generator

#include <glib.h>       // For various glib stuff.
#include <gio/gio.h>    // For the GDBus functions.

#include <escheme.h>    // For all the fun Scheme stuff
#include <scheme.h>     // For more fun Scheme stuff


// +--------+---------------------------------------------------------
// | Macros |
// +--------+

#ifdef VERBOSE
#define LOG(FORMAT, ARGS...) \
  do { \
    fprintf (stderr, "\t *** "); \
    fprintf (stderr, FORMAT, ## ARGS); \
    fprintf (stderr, "\n"); \
  } while (0)
#define SCHEME_LOG(MSG,OBJ) loudbus_log_scheme_object (MSG, OBJ)
#else
#define LOG(FORMAT, ARGS...) do { } while (0)
#define SCHEME_LOG(MSG,OBJ) do { } while (0)
#endif


// +-------+----------------------------------------------------------
// | Types |
// +-------+

/**
 * The information we store for a proxy.  In addition to the main
 * proxy, we also need information on the proxy, so that we can
 * look up information.
 */
struct LouDBusProxy
  {
    int signature;              // Identifies this as a proxy
    GDBusProxy *proxy;          // The real proxy
    GDBusNodeInfo *ninfo;       // Information on the proxy
    GDBusInterfaceInfo *iinfo;  // Information on the interace, used
                                // to extract info about param. types
  };
typedef struct LouDBusProxy LouDBusProxy;


// +---------+--------------------------------------------------------
// | Globals |
// +---------+

/**
 * A Scheme object to tag proxies.
 */
static Scheme_Object *LOUDBUS_PROXY_TAG;


// +--------------------------+---------------------------------------
// | Selected Predeclarations |
// +--------------------------+

static Scheme_Object *loudbus_call_with_closure (int argc, 
                                                 Scheme_Object **argv, 
                                                 Scheme_Object *prim);

static void loudbus_proxy_free (LouDBusProxy *proxy);

static int g_dbus_method_info_num_formals (GDBusMethodInfo *method);

static GVariant *scheme_object_to_parameter (Scheme_Object *obj, gchar *type);

static LouDBusProxy *scheme_object_to_proxy (Scheme_Object *obj);

static char *scheme_object_to_string (Scheme_Object *scmval);

static int loudbus_proxy_validate (LouDBusProxy *proxy);


// +---------------------------------------+--------------------------
// | Bridges Between LouDBusProxy and Racket |
// +---------------------------------------+

/**
 * Finalize a loudbus_proxy.  
 */
static void
loudbus_proxy_finalize (void *p, void *data)
{
  LouDBusProxy *proxy;
  LOG ("loudbus_proxy_finalize (%p,%p)", p, data);
  proxy = scheme_object_to_proxy (p);
  loudbus_proxy_free (proxy);
} // loudbus_proxy_finalize


// +-----------------+------------------------------------------------
// | Local Utilities |
// +-----------------+

/**
 * Convert underscores to dashes in a string.
 */
static void
dash_it_all (gchar *str)
{
  while (*str != '\0')
    {
      if (*str == '_')
        *str = '-';
      str++;
    } // while
} // dash_it_all

/**
 * Log a message and a scheme object.
 */
static void
loudbus_log_scheme_object (gchar *msg, Scheme_Object *obj)
{
  gchar *rendered;
  rendered = scheme_display_to_string (obj, NULL);
  fprintf (stderr, "%s: %s\n", msg, rendered);
} // loudbus_log_scheme_object

/**
 * Get the signature used to identify LouDBusProxy objects.
 */
static int
loudbus_proxy_signature (void)
{
  static int signature = 0;     // The signature.

  // Get a non-zero signature.
  while (signature == 0)
    {
      signature = random ();
    } // while (signature == 0)

  return signature;
} // loudbus_proxy_signature

/**
 * Get information on the proxy.
 */
static GDBusNodeInfo *
g_dbus_proxy_get_node_info (GDBusProxy *proxy)
{
  GError *error;                // Error returned by various functions.
  GVariant *response;           // The response from the proxy call.
  GDBusNodeInfo *info;          // Information on the node.
  const gchar *xml;             // XML code for the proxy interface.

  // Get the introspection data
  error = NULL;
  response = 
    g_dbus_proxy_call_sync (proxy, 
                            "org.freedesktop.DBus.Introspectable.Introspect",
                            NULL, 
                            G_DBUS_CALL_FLAGS_NONE,
                            -1,
                            NULL,
                            &error);
  if (response == NULL)
    {
      return NULL;
    } // if (response == NULL)

  // Get the XML from the introspection data
  g_variant_get (response, "(&s)", &xml);

  // Build an object that lets us explore the introspection data.
  error = NULL;
  info = g_dbus_node_info_new_for_xml (xml, &error);
  g_variant_unref (response);
  if (info == NULL)
    {
      return NULL;
    } // if (info == NULL)

  // And return that object
  return info;
} // g_dbus_proxy_get_node_info

/**
 * Determine the length of a null-terminated array of pointers.
 */
static int
parray_len (gpointer *arr)
{
  int i;                // Index into the array
  if (arr == NULL)
    return 0;
  for (i = 0; arr[i] != NULL; i++)
    ;
  return i;
} // parray_len

/**
 * Register a scheme function.  Provides a slightly more concise interface
 * to a few lines that we type regularly.
 */
static void
register_function (Scheme_Prim *prim, gchar *name, 
                   int minarity, int maxarity,
                   Scheme_Env *menv)
{
  Scheme_Object *proc = 
    scheme_make_prim_w_arity (prim, name, minarity, maxarity);
  scheme_add_global (name, proc, menv);
} // register_function

/**
 * Convert dashes to underscores in a string.
 */
static void
score_it_all (gchar *str)
{
  while (*str != '\0')
    {
      if (*str == '-')
        *str = '_';
      str++;
    } // while
} // score_it_all


// +-----------------+------------------------------------------------
// | Proxy Functions |
// +-----------------+

/**
 * Free one of the allocated proxies.
 */
void
loudbus_proxy_free (LouDBusProxy *proxy)
{
  // Sanity check 1.  Make sure that it's not NULL.
  if (proxy == NULL)
    return;

  // Sanity check 2.  Make sure that it's really an LouDBusProxy.
  if (! loudbus_proxy_validate (proxy))
    return;
 
  // Clear the signature (so that we don't identify this as a
  // LouDBusProxy in the future).
  proxy->signature = 0;

  // Clear the proxy.
  if (proxy->proxy != NULL)
    {
      g_object_unref (proxy->proxy);
      proxy->proxy = NULL;
    } // if (proxy->proxy != NULL)

  // Clear the node information.
  if (proxy->ninfo != NULL)
    {
      g_dbus_node_info_unref (proxy->ninfo);
      proxy->ninfo = NULL;
    } // if (proxy->ninfo != NULL)

  // Clear the interface information
  proxy->iinfo = NULL;  // Part of the node info, so not freed separately.

  // And free the enclosing structure
  g_free (proxy);
} // loudbus_proxy_free

LouDBusProxy *
loudbus_proxy_new (gchar *service, gchar *object, gchar *interface, 
                   GError **errorp)
{
  LouDBusProxy *proxy;         // The proxy we're creating

  // Allocate space for the struct.
  proxy = g_malloc0 (sizeof (LouDBusProxy));
  if (proxy == NULL)
    {
      LOG ("loudbus_proxy_new: Could not allocate proxy.");
      return NULL;
    } // if (proxy == NULL)

  LOG ("Creating proxy for (%s,%s,%s)", service, object, interface);
  proxy->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                NULL,
                                                service,
                                                object,
                                                interface,
                                                NULL,
                                                errorp);
  if (proxy->proxy == NULL)
    {
      LOG ("loudbus_proxy_new: Could not build proxy.");
      g_free (proxy);
      return NULL;
    } // if we failed to create the proxy.

  // Get the node information
  proxy->ninfo = g_dbus_proxy_get_node_info (proxy->proxy);
  if (proxy->ninfo == NULL)
    {
      LOG ("loudbus_proxy_new: Could not get node info.");
      g_object_unref (proxy->proxy);
      g_free (proxy);
      return NULL;
    } // if we failed to get node information

  // Get the interface information
  proxy->iinfo = g_dbus_node_info_lookup_interface(proxy->ninfo, interface);
  if (proxy->iinfo == NULL)
    {
      LOG ("loudbus_proxy_new: Could not get interface info.");
      g_object_unref (proxy->proxy);
      g_dbus_node_info_unref (proxy->ninfo);
      g_free (proxy);
      return NULL;
    } // if we failed to get interface information

  // We will be looking stuff up in the interface, so build a cache
  g_dbus_interface_info_cache_build (proxy->iinfo);

  // Set the signature
  proxy->signature = loudbus_proxy_signature ();

  // And we seem to be done
  return proxy;
} // loudbus_proxy_new

int
loudbus_proxy_validate (LouDBusProxy *proxy)
{
  // Sanity check.  We don't want segfaults.
  if (proxy == NULL)
    return 0;

  // Things are only proxies if they contain the magic signature.
  return (proxy->signature == loudbus_proxy_signature ());
} // loudbus_proxy_validate


// +-----------------+------------------------------------------------
// | Type Conversion |
// +-----------------+

/**
 * Convert a D-Bus signature to a human-readable string.
 */
static gchar *
dbus_signature_to_string (gchar *signature)
{
  switch (signature[0])
    {
      case 'a':
        switch (signature[1])
          {
            case 'i':
              return "list/vector of integers";
            case 's':
              return "list/vector of strings";
            case 'y':
              return "bytes";
            default:
              return signature;
          } // inner switch
      case 'i':
        return "integer";
      case 's':
        return "string";
      case 'y':
        return "byte";
      default:
        return signature;
    } // switch
} // dbus_signature_to_string

/**
 * Convert a GVariant to a Scheme object.  Returns NULL if there's a
 * problem.
 */
static Scheme_Object *
g_variant_to_scheme_object (GVariant *gv)
{
  const GVariantType *type;     // The type of the GVariant
  const gchar *typestring;      // A string that describes the type
  int i;                        // A counter variable
  int len;                      // Length of arrays and tuples
  Scheme_Object *lst = NULL;    // A list that we build as a result
  Scheme_Object *sval = NULL;   // One value
  Scheme_Object *result = NULL; // One result to return.

  // Special case: We'll treat NULL as void.
  if (gv == NULL)
    {
      return scheme_void;
    } // if (gv == NULL)

  // Get the type
  type = g_variant_get_type (gv);
  typestring = g_variant_get_type_string (gv);

  // ** Handle most of the basic types **

  // Integer
  if (g_variant_type_equal (type, G_VARIANT_TYPE_INT32))
    {
      // We don't refer to any Scheme objects across allocating calls,
      // so no need for GC code.
      int i;
      i = g_variant_get_int32 (gv);
      result = scheme_make_integer (i);
      return result;
    } // if it's an integer

  // Double
  if (g_variant_type_equal (type, G_VARIANT_TYPE_DOUBLE))
    {
      double d;
      d = g_variant_get_double (gv);
      result = scheme_make_double (d);
      return result;
    } // if it's a double

  // String
  if (g_variant_type_equal (type, G_VARIANT_TYPE_STRING))
    {
      // We don't refer to any Scheme objects across allocating calls,
      // so no need for GC code.
      const gchar *str;
      str = g_variant_get_string (gv, NULL);
      result = scheme_make_locale_string (str);
      return result;
    } // if it's a string

  // ** Handle some special cases **

  // We treat arrays of bytes as bytestrings
  if (g_strcmp0 (typestring, "ay") == 0)
    {
      gsize size;
      guchar *data;
      data = (guchar *) g_variant_get_fixed_array (gv, &size, sizeof (guchar));
      return scheme_make_sized_byte_string ((char *) data, size, 1);
    } // if it's an array of bytes

  // ** Handle the compound types ** 

  // Tuple or Array
  if ( (g_variant_type_is_tuple (type))
       || (g_variant_type_is_array (type)) )
    {
      // Find out how many values to put into the list.
      len = g_variant_n_children (gv);

      // Here, we are referring to stuff across allocating calls, so we
      // need to be careful.
      MZ_GC_DECL_REG (2);
      MZ_GC_VAR_IN_REG (0, lst);
      MZ_GC_VAR_IN_REG (1, sval);
      MZ_GC_REG ();
     
      // Start with the empty list.
      lst = scheme_null;

      // Step through the items, right to left, adding them to the list.
      for (i = len-1; i >= 0; i--)
        {
          sval = g_variant_to_scheme_object (g_variant_get_child_value (gv, i));
          lst = scheme_make_pair (sval, lst);
        } // for

          // Okay, we've made it through the list, now we can clean up.
      MZ_GC_UNREG ();
      if ((g_variant_type_is_array (type)))
        {
          //If type is array, convert to vector
          scheme_list_to_vector ((char*)lst);
        }//If array
      // And we're done.
      return lst;


    } // if it's a tuple or an array

  // Unknown.  Give up.
  scheme_signal_error ("Unknown type %s", typestring);
  return scheme_void;
} // g_variant_to_scheme_object

/**
 * Convert a Scheme list or vector to a GVariant that represents an array.
 */
static GVariant *
scheme_object_to_array (Scheme_Object *lv, gchar *type)
{
  Scheme_Object *sval;  // One element of the list/array
  GVariant *gval;       // The converted element
  GVariantBuilder *builder;

  // Special case: The empty list gives the empty array.
  if (SCHEME_NULLP (lv))
    {
      // Note: For individual objects, D-Bus type signatures are acceptable
      // as GVariant type strings.
      builder = g_variant_builder_new ((GVariantType *) type);
      if (builder == NULL)
        return NULL;
      return g_variant_builder_end (builder);
    } // if it's null

  // A list, or so we think.
  if (SCHEME_PAIRP (lv))
    {
      builder = g_variant_builder_new ((GVariantType *) type);
      if (builder == NULL)
        return NULL;
      // Follow the cons cells through the list
      while (SCHEME_PAIRP (lv))
        {
          sval = SCHEME_CAR (lv);
          gval = scheme_object_to_parameter (sval, type+1);
          if (gval == NULL)
            {
              g_variant_builder_unref (builder);
              return NULL;
            } // if (gval == NULL)
          g_variant_builder_add_value (builder, gval);
          lv = SCHEME_CDR (lv);
        } // while

      // We've reached the end.  Was it really a list?
      if (! SCHEME_NULLP (lv))
        {
          g_variant_builder_unref (builder);
          return NULL;
        } // If the list does not end in null, so it's not a list.

      // We've hit the null at the end of the list.
      return g_variant_builder_end (builder);
    } // if it's a list

  // A vector
  else if (SCHEME_VECTORP (lv))
    {
      int len = SCHEME_VEC_SIZE (lv);
      int i;

      LOG ("scheme_object_to_array: Handling a vector of length %d", len);

      builder = g_variant_builder_new (G_VARIANT_TYPE_ARRAY);
      if (builder == NULL)
        return NULL;

      for (i = 0; i < len; i++)
        {
          sval = SCHEME_VEC_ELS(lv)[i];
          gval = scheme_object_to_parameter (sval, type + 1);
          if (gval == NULL)
            {
              g_variant_builder_unref (builder);
              return NULL;
            } // if we could not convert the object
          g_variant_builder_add_value (builder, gval);
        } // for each index

      return g_variant_builder_end (builder);
    } // if it's a vector

  // Can only convert lists and vectors.
  else
    return NULL;
} // scheme_object_to_array

/**
 * Convert a Scheme object to a GVariant that will serve as one of
 * the parameters of a call go g_dbus_proxy_call_....  Returns NULL
 * if it is unable to do the conversion.
 */
static GVariant *
scheme_object_to_parameter (Scheme_Object *obj, gchar *type)
{
  gchar *str;           // A temporary string

  // Special case: Array of bytes
  if (g_strcmp0 (type, "ay") == 0) 
    {
      if (SCHEME_BYTE_STRINGP (obj))
        {
          return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                            SCHEME_BYTE_STR_VAL (obj),
                                            SCHEME_BYTE_STRLEN_VAL (obj),
                                            sizeof (guchar));
        } // if it's a byte string
    } // array of bytes

  // Handle normal cases
  switch (type[0])
    {
      // Arrays
      case 'a':
        return scheme_object_to_array (obj, type);

      // Doubles
      case 'd':
        if (SCHEME_DBLP (obj))
          return g_variant_new ("d", SCHEME_DBL_VAL (obj));
        else if (SCHEME_FLTP (obj))
          return g_variant_new ("d", (double) SCHEME_FLT_VAL (obj));
        else if (SCHEME_INTP (obj))
          return g_variant_new ("d", (double) SCHEME_INT_VAL (obj));
        else if (SCHEME_RATIONALP (obj))
          return g_variant_new ("d", (double) scheme_rational_to_double (obj));
        else
          return NULL;

      // 32 bit integers
      case 'i':
        if (SCHEME_INTP (obj))
          return g_variant_new ("i", (int) SCHEME_INT_VAL (obj));
        else if (SCHEME_DBLP (obj))
          return g_variant_new ("i", (int) SCHEME_DBL_VAL (obj));
        else if (SCHEME_FLTP (obj))
          return g_variant_new ("i", (int) SCHEME_FLT_VAL (obj));
        else if (SCHEME_RATIONALP (obj))
          return g_variant_new ("i", (int) scheme_rational_to_double (obj));
        else 
          return NULL;

      // Strings
      case 's':
        str = scheme_object_to_string (obj);
        if (str == NULL)
          return NULL;
        return g_variant_new ("s", str);

      // 32 bit unsigned integers
      case 'u':
        if (SCHEME_INTP (obj))
          return g_variant_new ("u", (unsigned int) SCHEME_INT_VAL (obj));
        else
          return NULL;

      // Everything else is currently unsupported
      default:
        return NULL;
    } // switch
} // scheme_object_to_parameter

/**
 * Convert a Scheme object representing an LouDBusProxy to the proxy.
 * Returns NULL if it cannot convert.
 */
static LouDBusProxy *
scheme_object_to_proxy (Scheme_Object *obj)
{
  LouDBusProxy *proxy;

  LOG ("scheme_object_to_proxy (%p)", obj);

  // Make sure that we have a pointer.
  if (! SCHEME_CPTRP (obj))
    {
      LOG ("scheme_object_to_proxy: not a pointer");
      return NULL;
    } // if the object is not a pointer

#ifdef CHECK_POINTER_TYPE
/*
  Note: I don't seem to have the right tag on the object, which
  suggests to me that the scheme_make_cptr doesn't quite work
  the way I expect.
 */
  // Make sure that Scheme thinks it's the right kind of pointer.
  if (SCHEME_CPTR_TYPE (obj) == LOUDBUS_PROXY_TAG)
    {
      LOG ("scheme_object_to_proxy: wrong type of pointer");
      return NULL;
    } // if the object has the wrong type
#endif

  // Get the pointer
  proxy = SCHEME_CPTR_VAL (obj);
  LOG ("scheme_object_to_proxy: potential proxy is %p", proxy);

  // Make sure that we also think that it's a proxy.
  if (! loudbus_proxy_validate (proxy))
    {
      LOG ("scheme_object_to_proxy: not really a proxy");
      return NULL;
    } // if it's not really a proxy

  LOG ("scheme_object_to_proxy: validated.");
  // And that's it
  return proxy;
} // scheme_object_to_proxy

/**
 * Given some kind of Scheme string value, convert it to a C string
 * If scmval is not a string value, returns NULL.
 */
static char *
scheme_object_to_string (Scheme_Object *scmval)
{
  char *str = NULL;

  // Char strings are the normal Scheme strings.  They need to be 
  // converted to byte strings.
  if (SCHEME_CHAR_STRINGP (scmval))
    {
      scmval = scheme_char_string_to_byte_string_locale (scmval);
      str = SCHEME_BYTE_STR_VAL (scmval);
    } // if it's a char string

  // Byte strings are easy, but not the typical Scheme strings.
  else if (SCHEME_BYTE_STRINGP (scmval))
    {
      str = SCHEME_BYTE_STR_VAL (scmval);
    } // if it's a byte string

  // A design decision: We'll treat symbols as strings.  (It certainly
  // makes things easier for the client.)
  else if (SCHEME_SYMBOLP (scmval))
    {
      str = SCHEME_SYM_VAL (scmval);
    } // if it's a symbol

  // Everything else is not a string
  else
    {
      // Signal an error by setting the return value to NULL.
      str = NULL; 
    } // if it's not a string
  return str;
} // scheme_object_to_string

/**
 * Convert an array of Scheme objects to a GVariant that serves as
 * the primary parameter to g_dbus_proxy_call.
 */
static GVariant *
scheme_objects_to_parameter_tuple (gchar *fun,
                                   int arity,
                                   Scheme_Object **objects,
                                   GDBusArgInfo *formals[])
{
  int i;                // Counter variable
  GVariantBuilder *builder;
                        // Something to let us build tuples
  GVariant *result;     // The GVariant we build
  GVariant *actual;     // One actual

  builder = g_variant_builder_new (G_VARIANT_TYPE_TUPLE);

  // Annotations for garbage collector.
  // Since we're converting Scheme_Object values to GVariants, it should
  // not be the case that we have an "allocating call".  However, I am
  // worried that conversion to a string, which requires
  // scheme_char_string_to_byte_string_locale, might be considered an
  // allocating call.  So let's be in the safe side.  The sample code suggests
  // that we can put an array of GObjects in a single variable (see
  // the supplied makeadder3m.c for more details).
  MZ_GC_DECL_REG (1);
  MZ_GC_VAR_IN_REG (0, objects);
  MZ_GC_REG ();

  // Process all the parameters
  for (i = 0; i < arity; i++)
    {
      actual = scheme_object_to_parameter (objects[i], formals[i]->signature);
      // If we can't convert the parameter, we give up.
      if (actual == NULL)
        {
          // Early exit - Clean up for garbage collection
          MZ_GC_UNREG ();
          // Get rid of the builder
          g_variant_builder_unref (builder);
          // And return an arror message.
          scheme_wrong_type (fun, 
                             dbus_signature_to_string (formals[i]->signature), 
                             i, 
                             arity, 
                             objects);
        } // If we could not convert
      // Otherwise, we add the value to the builder and go on
      g_variant_builder_add_value (builder, actual);
    } // for

  // Clean up garbage collection info.
  MZ_GC_UNREG ();
  // And we're done.
  result = g_variant_builder_end (builder);
  return result;
} // scheme_objects_to_parameter_tuple


// +-----------------------+------------------------------------------
// | Other Local Functions |
// +-----------------------+

/**
 * Add one of the procedures that the proxy provides on the D-Bus.
 */
static void
loudbus_add_dbus_proc (Scheme_Env *env, 
                       Scheme_Object *proxy, 
                       gchar *dbus_name, 
                       gchar *external_name,
                       int arity)
{
  Scheme_Object *vals[3];
  Scheme_Object *proc;
  vals[0] = NULL;
  vals[1] = NULL;
  vals[2] = NULL;

  // Prepare for potential garbage collection during allocating calls
  // (e.g., scheme_make_locale_string).
  MZ_GC_DECL_REG (3);
  MZ_GC_VAR_IN_REG (0, vals[0]);
  MZ_GC_VAR_IN_REG (1, vals[1]);
  MZ_GC_VAR_IN_REG (2, vals[2]);
  MZ_GC_REG ();

  // Fill in the closure with the object.
  vals[0] = proxy;
  vals[1] = scheme_make_locale_string (dbus_name);
  vals[2] = scheme_make_locale_string (external_name);

  // Build the procedure.  Note that we need to duplicate the
  // external name because scheme_make_prim_closure_w_arity seems
  // to retain a pointer to the string.  (At least, it seems that way
  // to me.)
  proc = scheme_make_prim_closure_w_arity (loudbus_call_with_closure, 
                                           3, vals, 
                                           g_strdup (external_name),
                                           arity, arity);

  // And add it to the environment.  
  scheme_add_global (external_name, proc, env);

  // And update the GC info.
  MZ_GC_UNREG ();
} // loudbus_add_dbus_proc

/**
 * The kernel of the various mechanisms for calling D-Bus functions.
 */
static Scheme_Object *
dbus_call_kernel (LouDBusProxy *proxy,
                  gchar *dbus_name,
                  gchar *external_name,
                  int argc, 
                  Scheme_Object **argv)
{
  GDBusMethodInfo *method;
                        // Information on the actual method
  int arity;            // The arity of that method
  GVariant *actuals;    // The actual parameters
  GVariant *gresult;    // The result from the function call as a GVariant
  Scheme_Object *sresult;   
                        // That Scheme result as a Scheme object
  GError *error;        // Possible error from call

  // Grab the method information.
  method = g_dbus_interface_info_lookup_method (proxy->iinfo, dbus_name);
  if (method == NULL)
    {
      scheme_signal_error ("no such method: %s", dbus_name);
    } // if the method is invalid

  // Get the arity
  arity = g_dbus_method_info_num_formals (method);
  if (arity != argc)
    {
      scheme_signal_error ("%s expected %d params, received %d",
                           external_name, arity, argc);
    } // if the arity is incorrect

  // Build the actuals
  actuals = scheme_objects_to_parameter_tuple (external_name,
                                               argc,
                                               argv,
                                               method->in_args);
  if (actuals == NULL)
    {
      scheme_signal_error ("%s: could not convert parameters",
                           external_name);
    } // if (actuals == NULL)

  // Call the function.
  error = NULL;
  gresult = g_dbus_proxy_call_sync (proxy->proxy,
                                    dbus_name,
                                    actuals,
                                    0,
                                    -1,
                                    NULL,
                                    &error);
  if (gresult == NULL)
    {
      if (error != NULL)
        {
          scheme_signal_error ("%s: call failed because %s", 
                               external_name, error->message);
        } // if (error != NULL)
      else
        {
          scheme_signal_error ("%s: call failed for unknown reason", 
                               external_name);
        } // if something went wrong, but there's no error
    } // if (gresult == NULL)

  // Convert to Scheme form
  sresult = g_variant_to_scheme_object (gresult);
  if (sresult == NULL)
    {
      scheme_signal_error ("%s: could not convert return values", 
                           external_name);
    } // if (sresult == NULL)

  // And we're done.
  return sresult;
} // dbus_call_kernel

/**
 * Get a count of the number of methods in an interface.
 */
int
g_dbus_interface_info_num_methods (GDBusInterfaceInfo *info)
{
  int m;        // Counter variable
  // Special case: No methods
  if (info->methods == NULL)
    return 0;
  // Normal case: Iterate until we find the NULL.
  for (m = 0; info->methods[m] != NULL; m++)
    ;
  // And we're done.
  return m;
} // g_dbus_interface_info_num_methods

/**
 * Get a count of the number of formal parameters to a method.
 */
static int
g_dbus_method_info_num_formals (GDBusMethodInfo *method)
{
  int i;        // Counter variable
  // Special case?: No formals
  if (method->in_args == NULL)
    return 0;
  // Normal case: Iterate until we find the NULL.
  for (i = 0; method->in_args[i] != NULL; i++)
    ;
  // And we're done
  return i;
} // g_dbus_method_info_num_formals


// +--------------------------+---------------------------------------
// | Wrapped Scheme Functions |
// +--------------------------+

/**
 * A general call.  Parameters are
 *  0: The LouDBusProxy
 *  1: The method name (string)
 *  others: Parameters to the method
 */
Scheme_Object *
loudbus_call (int argc, Scheme_Object **argv)
{
  LouDBusProxy *proxy;
  gchar *name;

  // I don't think that I need to add annotations for garbage collection
  // because scheme_object_to_string is the only allocating call, and we've
  // dealt with all the other Scheme objects by the time we call it.
  proxy = scheme_object_to_proxy (argv[0]);
  name = scheme_object_to_string (argv[1]);

  // Sanity checks
  if (proxy == NULL)
    {
      scheme_wrong_type ("loudbus-call", "LouDBusProxy *", 0, argc, argv);
    } // if we could not get the proxy
  if (name == NULL)
    {
      scheme_wrong_type ("loudbus-call", "string", 1, argc, argv);
    } // if we could not get the name

  // Permit the use of dashes
  score_it_all (name);

  return dbus_call_kernel (proxy, name, name, argc-2, argv+2);
} // loudbus_call

/**
 * Call a function, using the proxy, function name, and external name
 * stored in prim.
 *
 * argc/argv give the parameters for the function call.
 */
Scheme_Object *
loudbus_call_with_closure (int argc, Scheme_Object **argv, Scheme_Object *prim)
{
  Scheme_Object *wrapped_proxy = NULL;
  Scheme_Object *wrapped_dbus_name = NULL;
  Scheme_Object *wrapped_external_name = NULL;
  Scheme_Object *result = NULL;
  LouDBusProxy *proxy = NULL;
  gchar *dbus_name;
  gchar *external_name;

  // Probably too many things are annotated here, but better safe than
  // sorry.
  MZ_GC_DECL_REG (5);
  MZ_GC_VAR_IN_REG (0, argv);
  MZ_GC_VAR_IN_REG (1, prim);
  MZ_GC_VAR_IN_REG (2, wrapped_proxy);
  MZ_GC_VAR_IN_REG (3, wrapped_dbus_name);
  MZ_GC_VAR_IN_REG (4, wrapped_external_name);
  MZ_GC_REG ();

  // Extract information from the closure.
  wrapped_proxy = SCHEME_PRIM_CLOSURE_ELS (prim)[0];
  wrapped_dbus_name = SCHEME_PRIM_CLOSURE_ELS (prim)[1];
  wrapped_external_name = SCHEME_PRIM_CLOSURE_ELS (prim)[2];
  dbus_name = scheme_object_to_string (wrapped_dbus_name);
  external_name = scheme_object_to_string (wrapped_external_name);
  proxy = scheme_object_to_proxy (wrapped_proxy);

  // Sanity check
  if (proxy == NULL)
    {
      MZ_GC_UNREG ();
      scheme_signal_error ("Could not obtain proxy to call %s.\n",
                           external_name);
    } // if (proxy == NULL)
   
  // And do the dirty work
  result = dbus_call_kernel (proxy, 
                             dbus_name, external_name, 
                             argc, argv);

  MZ_GC_UNREG ();
  return result;
} // loudbus_call_with_closure

/**
 * Import all of the methods from a LouDBusProxy.
 */
Scheme_Object *
loudbus_import (int argc, Scheme_Object **argv)
{
  Scheme_Env *env = NULL;       // The environment
  GDBusMethodInfo *method;      // Information on one method
  LouDBusProxy *proxy;            // The proxy
  int m;                        // Counter variable for methods
  int n;                        // The total number of methods
  int arity;                    // The arity of a method
  gchar *prefix = NULL;         // The prefix we use
  gchar *external_name;         // The name we use in Scheme
  int dashes;                   // Convert underscores to dashes?

  // Annotations and other stuff for garbage collection.
  MZ_GC_DECL_REG (3);
  MZ_GC_VAR_IN_REG (0, argv);
  MZ_GC_VAR_IN_REG (1, env);
  MZ_GC_VAR_IN_REG (2, prefix);
  MZ_GC_REG ();

  // Get the proxy
  proxy = scheme_object_to_proxy (argv[0]);
  if (proxy == NULL)
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-import", "LouDBusProxy *", 0, argc, argv);
    } // if (proxy == NULL)

  // Get the prefix
  prefix = scheme_object_to_string (argv[1]);
  if (prefix == NULL)
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-import", "string", 1, argc, argv);
    } // if (prefix == NULL)

  // Get the flag
  if (! SCHEME_BOOLP (argv[2]))
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-import", "Boolean", 2, argc, argv);
    } // if (!SCHEME_BOOLB (argv[2])
  dashes = SCHEME_TRUEP (argv[2]);

  // Get the current environment, since we're mutating it.
  env = scheme_get_env (scheme_current_config ());

  // Process the methods
  n = g_dbus_interface_info_num_methods (proxy->iinfo);
  for (m = 0; m < n; m++)
    {
      method = proxy->iinfo->methods[m];
      arity = g_dbus_method_info_num_formals (method);
      external_name = g_strdup_printf ("%s%s", prefix, method->name);
      if (external_name != NULL)
        {
          if (dashes)
            {
              dash_it_all (external_name);
            } // if (dashes)

          // And add the procedure
          LOG ("loudbus-import: adding %s as %s", method->name, external_name);
          loudbus_add_dbus_proc (env, argv[0], 
                                 method->name, external_name, 
                                 arity);
          // Clean up
          g_free (external_name);
        } // if (external_name != NULL)
    } // for each method

  // And we're done.
  MZ_GC_UNREG ();
  return scheme_void;
} // loudbus_import

/**
 * Initialize the louDBus library by getting the appropriate Scheme_Object to
 * name pointers.
 */
Scheme_Object *
loudbus_init (int argc, Scheme_Object **argv)
{
  int size;
  // No allocation, so no GC annotations necessary.
  LOUDBUS_PROXY_TAG = argv[0];
  size = sizeof (*LOUDBUS_PROXY_TAG);
  LOG ("loudbus_init: I think that the size of LOUDBUS_PROXY_TAG is %d.\n", size);
  scheme_register_static (LOUDBUS_PROXY_TAG, size);
  return scheme_void;
} // loudbus_init

/**
 * Get information on one method (annotations, parameters, return
 * values, etc).
 *
 * TODO:
 *   1. Add missing documentation (see ????)
 *   2. Check to make sure that the second parameter is a string.  (Also
 *      do other error checking.  See below.)
 *   3. Make sure that you get annotations for parameters and return
 *      values (if they exist).
 *   4. Add tags for the other parts of the record (if they aren't
 *      there already).  For example, something like
 *      '((name gimp_image_new)
 *        (annotations "...")
 *        (inputs (width integer "width of image"))
 *        (outputs (image integer "id of created image")))
 *      If you'd prefer, input and output could also have their own
 *      tags.
 *        (inputs ((name width) (type integer) (annotations "width of image")))
 *   5. Add a function to louDBus/unsafe that pretty prints this.  
 *      (If you'd prefer, you can add it to this file.  But you can't
 *      use printf to pretty print.)
 *   6. Add information for the garbage collector.  (Yup, you'll need to
 *      read really bad documentation on this.  But try.)
 */
static Scheme_Object *
loudbus_method_info (int argc, Scheme_Object **argv)
{
  Scheme_Object *val, *val2;            // ????
  Scheme_Object *result = NULL;         // The result we're building
  Scheme_Object *arglist = NULL;        // The list of arguments
  Scheme_Object *outarglist = NULL;     // The list of return values
  Scheme_Object *annolist = NULL;       // The list of annotations   
  Scheme_Object *name = NULL;           // The method's name
  Scheme_Object *parampair = NULL;      // ????
  Scheme_Object *outparampair = NULL;   // ????
  GDBusMethodInfo *method;              // Information on one method
  GDBusAnnotationInfo *anno;            // Information on the annotations
  GDBusArgInfo *args, *outargs;         // Information on the arguments
  LouDBusProxy *proxy;                  // The proxy
  gchar *methodName;                    // The method name
  int m;                                // Counter variable for methods

  // Get the proxy
  proxy = scheme_object_to_proxy (argv[0]);
  if (proxy == NULL)
    {
      scheme_wrong_type ("loudbus-methods", "LouDBusProxy *", 0, argc, argv);
    } // if proxy == NULL

  //Get the method name.  WHAT IF WE CAN'T CONVERT TO A STRING????
  methodName = scheme_object_to_string (argv[1]);

  // Permit the use of dashes in method names by converting them back
  // to underscores (which is what we use over DBus).
  score_it_all (methodName);

  //Get the method struct.  WHAT IF THE METHOD DOESN'T EXIST????
  method = g_dbus_interface_info_lookup_method (proxy->iinfo, methodName);

  // Build the list for arguments.
  arglist = scheme_null;
  for (m = parray_len ((gpointer *) method->in_args) - 1; m >= 0; m--)
    {
      args = method->in_args[m]; //Go through the arguments.
      val = scheme_make_symbol (args->name);
      val2 = scheme_make_symbol (args->signature);
      parampair = scheme_make_pair (val, val2);
      arglist = scheme_make_pair (parampair, arglist);
    } // for each argument
 
  //Build list for output.
  outarglist = scheme_null;
  for (m = parray_len ((gpointer *) method->out_args) - 1; m >= 0; m--)
    {
      outargs = method->out_args[m];
      val = scheme_make_symbol (outargs->name);
      val2 = scheme_make_symbol (outargs->signature);
      outparampair = scheme_make_pair (val, val2);
      outarglist =  scheme_make_pair (outparampair, outarglist);
    } // for each output formals

  // Build list of annotations
  annolist = scheme_null;
  for (m = parray_len ((gpointer *) method->annotations) - 1; m >= 0; m--)
    {
      anno = method->annotations[m]; //Go through the annotations.
      val = scheme_make_locale_string (anno->value);
      annolist = scheme_make_pair (val, annolist);
    } // for each annotation

  // Create the name entry
  name = scheme_null;
  name = scheme_make_pair (scheme_make_symbol(methodName), name);
  name = scheme_make_pair (scheme_make_symbol("name"), name);

  result = scheme_null;
  result = scheme_make_pair (annolist, result);
  result = scheme_make_pair (outarglist, result);
  result = scheme_make_pair (arglist, result);
  result = scheme_make_pair (name, result);

  // And we're done.
  return result;
} // loudbus_method_info

/**
 * Get all of the methods from a louDBus Proxy.
 */
Scheme_Object *
loudbus_methods (int argc, Scheme_Object **argv)
{
  Scheme_Object *result = NULL; // The result we're building
  Scheme_Object *val = NULL;    // One method in the result
  GDBusMethodInfo *method;      // Information on one method
  LouDBusProxy *proxy;            // The proxy
  int m;                        // Counter variable for methods

  MZ_GC_DECL_REG (2);
  MZ_GC_VAR_IN_REG (0, result);
  MZ_GC_VAR_IN_REG (1, val);

  // Get the proxy
  proxy = scheme_object_to_proxy (argv[0]);
  if (proxy == NULL)
    {
      scheme_wrong_type ("loudbus-methods", "LouDBusProxy *", 0, argc, argv);
    } // if proxy == NULL

  MZ_GC_REG ();

  // Build the list.  
  result = scheme_null;
  for (m = g_dbus_interface_info_num_methods (proxy->iinfo) - 1; m >= 0; m--)
    {
      method = proxy->iinfo->methods[m];
      val = scheme_make_locale_string (method->name);
      result = scheme_make_pair (val, result);
    } // for each method

  MZ_GC_UNREG ();

  // And we're done.
  return result;
} // loudbus_methods

/**
 * Get a list of available objects.
 * TODO:
 *   1. Check that this works at all. (Nope.)
 *   2. Check that this gets the full path.  (Didn't we decide that
 *      you needed to recursively find elements?)
 *   3. Add error checking for the call to g_variant_to_scheme_result.
 *   4. Clean up after yourself.  You've created a proxy.  Get rid of
 *      it so it doesn't sit there clogging memory.  (See loudbus_proxy_free
 *      for details.)
 */
static Scheme_Object *
loudbus_objects (int argc, Scheme_Object **argv)
{
  GDBusProxy *proxy;            // Proxy for connecting to server
  GError *error;                // Potential error
  GVariant *params;             // Parameters to function call
  GVariant *result;             // Result of request for info
  gchar *service;               // Name of the service

  service = scheme_object_to_string (argv[0]);

  // Check parameter
  if (service == NULL)
    {
      scheme_wrong_type ("loudbus-proxy", "string", 0, argc, argv);
    } // if (service == NULL)
  
  // Create the proxy that we'll use to get information on the service.
  LOG ("Creatign proxy for %s", service);
  proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         NULL,
                                         service,
                                         "",
                                         "",
                                         NULL,
                                         &error);

  // Call the function to get the objects.  (This looks wrong.)
  params = g_variant_new("()");
  result = g_dbus_proxy_call_sync (proxy,
                                    "",
                                    params,
                                    0,
                                    -1,
                                    NULL,
                                    &error);

  // Check the result.
  // TODO

  // And we're done.
  return g_variant_to_scheme_object (result);
} // loudbus_objects

/**
 * Create a new proxy.
 */
static Scheme_Object *
loudbus_proxy (int argc, Scheme_Object **argv)
{
  gchar *service = NULL;        // A string giving the service
  gchar *path = NULL;           // A string giving the path to the object
  gchar *interface = NULL;      // A string giving the interface
  LouDBusProxy *proxy = NULL;   // The proxy we build
  Scheme_Object *result = NULL; // The proxy wrapped as a Scheme object
  GError *error = NULL;         // A place to hold errors

  service = scheme_object_to_string (argv[0]);

  // Annotations for garbage collection
  MZ_GC_DECL_REG (5);
  MZ_GC_VAR_IN_REG (0, argv);
  MZ_GC_VAR_IN_REG (1, service);
  MZ_GC_VAR_IN_REG (2, path);
  MZ_GC_VAR_IN_REG (3, interface);
  MZ_GC_VAR_IN_REG (4, result);
  MZ_GC_REG ();

  // Extract parameters
  service = scheme_object_to_string (argv[0]);
  path = scheme_object_to_string (argv[1]);
  interface = scheme_object_to_string (argv[2]);

  // Check parameters
  if (service == NULL)
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-proxy", "string", 0, argc, argv);
    }
  if (path == NULL)
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-proxy", "string", 1, argc, argv);
    }
  if (interface == NULL)
    {
      MZ_GC_UNREG ();
      scheme_wrong_type ("loudbus-proxy", "string", 2, argc, argv);
    }

  // Do the actual work in building the proxy.
  proxy = loudbus_proxy_new (service, path, interface, &error);
  if (proxy == NULL)
    {
      if (error == NULL)
        {
          MZ_GC_UNREG ();
          scheme_signal_error ("loudbus-proxy: "
                               "Could not create proxy for an unknown reason.");
        }
      else
        {
          MZ_GC_UNREG ();
          scheme_signal_error ("loudbus-proxy: "
                               "Could not create proxy because %s", 
                               error->message);
        }
    } // if (proxy == NULL)
  
  // Wrap the proxy into a Scheme type
  result = scheme_make_cptr (proxy, LOUDBUS_PROXY_TAG);

  // Log info during development
  LOG ("loudbus_proxy: Built proxy %p, Scheme object %p", proxy, result);

  // Find out information on what we just built.
  SCHEME_LOG ("result is", result);
  SCHEME_LOG ("result type is", SCHEME_CPTR_TYPE (result));
  
  // Register the finalizer
  scheme_register_finalizer (result, loudbus_proxy_finalize, NULL, NULL, NULL);

  // And we're done
  MZ_GC_UNREG ();
  return result;
} // loudbus_proxy

/**
 * Create a list of available services.
 */
static Scheme_Object *
loudbus_services (int argc, Scheme_Object **argv)
{
  GDBusProxy *proxy;
  GError *error = NULL; 
  GVariant *result;

  //Build the proxy.
  proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         NULL,
                                         "org.freedesktop.DBus",
                                         "/",
                                         "org.freedesktop.DBus",
                                         NULL,
                                         &error);

  // Check for an error
  if (proxy == NULL)
    {
      if (error != NULL)
        {
          scheme_signal_error ("Could not create proxy because %s", 
                               error->message);
        } // if (error != NULL)
      else // if (error == NULL)
        {
	  scheme_signal_error ("Could not create proxy for unknown reasons.");
	} // if (error == NULL)
      return scheme_void;
    } // if (proxy == NULL)

  // Get a list of available services.
  result = g_dbus_proxy_call_sync (proxy,
                                   "ListNames",
                                   NULL,
                                   0,
                                   -1,
                                   NULL,
                                   &error);

  // Check whether an error occurred.
  if (result == NULL)
    {
      if (error != NULL)
        {
	  scheme_signal_error ("Could not list services because: %s",
	                       error->message);
        } // if (error != NULL)
      else // if (error == NULL)
        {
	  scheme_signal_error ("Could not list services for unknown reason");
        } // if (error == NULL)
      return scheme_void; 
    } // if (error == NULL)
  
  // Return the created list.
  return g_variant_to_scheme_object (result);
} // loudbus_services


// +-----------------------+------------------------------------------
// | Standard Scheme Setup |
// +-----------------------+

Scheme_Object *
scheme_reload (Scheme_Env *env)
{
  Scheme_Env *menv = NULL;      // The module's environment

  // Annotations for the garbage collector
  MZ_GC_DECL_REG (2);
  MZ_GC_VAR_IN_REG (0, env);
  MZ_GC_VAR_IN_REG (1, menv);
  MZ_GC_REG ();

  // Build the module environment.
  menv = scheme_primitive_module (scheme_intern_symbol ("loudbus"),
                                  env);

  // Build the procedures
  register_function (loudbus_call,        "loudbus-call",        2, -1, menv);
  register_function (loudbus_import,      "loudbus-import",      3,  3, menv);
  register_function (loudbus_init,        "loudbus-init",        1,  1, menv);
  register_function (loudbus_method_info, "loudbus-method-info", 2,  2, menv);
  register_function (loudbus_methods,     "loudbus-methods",     1,  1, menv);
  register_function (loudbus_objects,     "loudbus-objects",     1,  1, menv);
  register_function (loudbus_proxy,       "loudbus-proxy",       3,  3, menv);
  register_function (loudbus_services,    "loudbus-services",    0,  0, menv);

  // And we're done.
  scheme_finish_primitive_module (menv);
  MZ_GC_UNREG ();

  return scheme_void;
} // scheme_reload

Scheme_Object *
scheme_initialize (Scheme_Env *env)
{
  // Seed our random number generator (but only once)
  srandom (time (NULL));      

  // Although g_type_init is deprecated since GLIB 2.36, it seems to be 
  // needed in the version of GLib we have installed in MathLAN.
  LOG ("GLIB %d.%d.%d", 
       GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
  if ((GLIB_MAJOR_VERSION == 2) && (GLIB_MINOR_VERSION < 36)) 
    {
      g_type_init ();
    } // if before 2.36

  return scheme_reload (env);
} // scheme_initialize

Scheme_Object *
scheme_module_name (void)
{
  return scheme_intern_symbol ("loudbus");
} // scheme_module_name
