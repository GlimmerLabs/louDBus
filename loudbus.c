/**
 * loudbus.c
 *   A D-Bus Client for Racket.
 *
 * Copyright (c) 2012 Zarni Htet, Samuel A. Rebelsky, Hart Russell, and
 * Mani Tiwaree.  All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the Lesser GNU General Public License as published 
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
x * GNU General Public License for more details.
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

#define DEBUG(...) do { fprintf (stderr, __VA_ARGS__); fprintf (stderr, "\n"); } while (0)


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

// +--------------------+---------------------------------------------
// | Exported Variables |
// +--------------------+

/**
 * A Scheme object to tag proxies.
 */
static Scheme_Object *ADBC_PROXY_TAG;


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

static char * scheme_object_to_string (Scheme_Object *scmval);

static int loudbus_proxy_validate (LouDBusProxy *proxy);


// +-----------------------------------------+------------------------
// | Bridges Between LouDBusProxy and Racket |
// +-----------------------------------------+

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
// | Local Functions |
// +-----------------+

/**
 * Convert underscores to dashess in a string.
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
      // I shouldn't reseed the random number generator each time,
      // but it's unlikely this loop will run more than once.
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
 
  // Clear the signature.
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

#ifdef HIDDEN
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
#endif

  // We will be looking stuff up in the interface, so build a cache
  if (proxy->iinfo != NULL)
    {
      g_dbus_interface_info_cache_build (proxy->iinfo);
    }

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
            default:
              return signature;
          } // inner switch
      case 'i':
        return "integer";
      case 's':
        return "string";
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

      // And we're done.
      return lst;
    } // if it's a tuple or an array

  // Unknown.  Give up.
  return NULL;
} // g_variant_to_scheme_object

/**
 * Convert a g_variant to a Scheme return value.
 */
static Scheme_Object *
g_variant_to_scheme_result (GVariant *gv)
{
  const GVariantType *type;     // The type of the GVariant

  // Special case: Singleton tuple.
  type = g_variant_get_type (gv);
  if ( (g_variant_type_is_tuple (type))
       && (g_variant_n_children (gv) == 1) )
    return g_variant_to_scheme_object (g_variant_get_child_value (gv, 0));

  // Normal case
  else
    return g_variant_to_scheme_object (gv);
} // g_variant_to_scheme_result

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
        else
          return NULL;

      // 32 bit integers
      case 'i':
        if (SCHEME_INTP (obj))
          return g_variant_new ("i", (int) SCHEME_INT_VAL (obj));
        else if (SCHEME_DBLP (obj))
          return g_variant_new ("i", (int) SCHEME_DBL_VAL (obj));
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
  if (SCHEME_CPTR_TYPE (obj) == ADBC_PROXY_TAG)
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
  // Byte strings are easy, but not the typical Scheme strings.
  if (SCHEME_BYTE_STRINGP (scmval))
    {
      str = SCHEME_BYTE_STR_VAL (scmval);
    } // if it's a byte string

  // Char strings are the normal Scheme strings.  They need to be 
  // converted to byte strings.
  else if (SCHEME_CHAR_STRINGP (scmval))
    {
      scmval = scheme_char_string_to_byte_string_locale (scmval);
      str = SCHEME_BYTE_STR_VAL (scmval);
    } // if it's a char string

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
  sresult = g_variant_to_scheme_result (gresult);
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



int
g_dbus_methods_info_num_annotations (GDBusMethodInfo *info)
{
  int m;        // Counter variable
  // Special case: No annotations
  if (info->annotations == NULL)
    return 0;
  // Normal case: Iterate until we find the NULL.
  for (m = 0; info->annotations[m] != NULL; m++)
    ;
  // And we're done.
  return m;
} // g_dbus_interface_info_num_methods


/**
 * Get a count of the number of formal parameters to a method.
 */
int
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


int
g_dbus_method_info_num_outFormals (GDBusMethodInfo *method)
{
  int i;        // Counter variable
  // Special case?: No formals
  if (method->out_args == NULL)
    return 0;
  // Normal case: Iterate until we find the NULL.
  for (i = 0; method->out_args[i] != NULL; i++)
    ;
  // And we're done
  return i;
} // g_dbus_method_info_num_outFormals

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
  ADBC_PROXY_TAG = argv[0];
  size = sizeof (*ADBC_PROXY_TAG);
  LOG ("loudbus_init: I think that the size of ADBC_PROXY_TAG is %d.\n", size);
  scheme_register_static (ADBC_PROXY_TAG, size);
  return scheme_void;
} // loudbus_init

/**
 * Get all of the methods from an ADBC Proxy.
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
 * Create a new proxy.
 */
static Scheme_Object *
loudbus_proxy (int argc, Scheme_Object **argv)
{
  gchar *service = NULL;        // A string giving the service
  gchar *path = NULL;           // A string giving the path to the object
  gchar *interface = NULL;      // A string giving the interface
  LouDBusProxy *proxy = NULL;     // The proxy we build
  Scheme_Object *result = NULL; // The proxy wrapped as a Scheme object
  GError *error = NULL;         // A place to hold errors

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
  result = scheme_make_cptr (proxy, ADBC_PROXY_TAG);

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


static Scheme_Object *
loudbus_method_info (int argc, Scheme_Object **argv)
{
  Scheme_Object *val, *val2;
  Scheme_Object *result = NULL;             // The result we're building
  Scheme_Object *arglist = NULL;           // The list of arguments
  Scheme_Object *outarglist = NULL;     
  Scheme_Object *annolist = NULL;         // The list of annotations   
  Scheme_Object *name = NULL;            // The method's name
  Scheme_Object *parampair = NULL;
  Scheme_Object *outparampair = NULL;
  GDBusMethodInfo *method;             // Information on one method
  GDBusAnnotationInfo *anno;          // The information on the annotations
  GDBusArgInfo *args, *outargs;      // The information on the arguments
  LouDBusProxy *proxy;              // The proxy
  gchar *methodName;               // The method name
  int m;                          // Counter variable for methods

  // Get the proxy
  proxy = scheme_object_to_proxy (argv[0]);
  if (proxy == NULL)
    {
      scheme_wrong_type ("loudbus-methods", "LouDBusProxy *", 0, argc, argv);
    } // if proxy == NULL

  //Get the method name
  methodName = scheme_object_to_string (argv[1]);

  //Get the method struct
  method = g_dbus_interface_info_lookup_method (proxy->iinfo, methodName);

  // Build the list for arguments.
 
  arglist = scheme_null;
  for (m = g_dbus_method_info_num_formals (method) - 1; m >= 0; m--)
    {
      args = method->in_args[m]; //Go through the arguments.
      val = scheme_make_symbol (args->name);
      val2 = scheme_make_symbol (args->signature);
      parampair = scheme_make_pair (val, val2);
      arglist = scheme_make_pair (parampair, arglist);
    } // for each method
 

  //Build list for outpu  //Build list for output.

  outarglist = scheme_null;
  for (m = g_dbus_method_info_num_outFormals (method) - 1; m >= 0; m--)
    {
      outargs = method->out_args[m];
      val = scheme_make_symbol (outargs->name);
      val2 = scheme_make_symbol (outargs->signature);
      outparampair = scheme_make_pair (val, val2);
      outarglist =  scheme_make_pair (outparampair, outarglist);
    }

  //Build list for annotations.

  annolist = scheme_null;
  for (m = g_dbus_methods_info_num_annotations (method) - 1; m >= 0; m--)
    {
      anno = method->annotations[m]; //Go through the annotations.
      val = scheme_make_locale_string(anno->value);
      annolist = scheme_make_pair(val, annolist);
    } // for each method

  name = scheme_null;
  name = scheme_make_pair (scheme_make_symbol(methodName), name);
  name = scheme_make_pair (scheme_make_symbol("Name:"), name);

  result = scheme_null;
  result = scheme_make_pair (annolist, result);
  result = scheme_make_pair (outarglist, result);
  result = scheme_make_pair (arglist, result);
  result = scheme_make_pair (name, result);

  // And we're done.
  return result;


} // loudbus_method_info


static Scheme_Object *
loudbus_services (int argc, Scheme_Object **argv)
{
  LouDBusProxy *proxy;
  GError *error = NULL; 
  GVariant *gresult;

  //Build the proxy.
  proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         NULL,
                                         "org.freedesktop.DBus",
                                         "/",
                                         "org.freedesktop.DBus",
                                         NULL,
                                         &error);

  gresult = g_dbus_proxy_call_sync (proxy,
                                    "ListNames",
                                    NULL,
                                    0,
                                    -1,
                                    NULL,
                                    &error);

  // Check whether an error occurred.
  if (gresult == NULL)
    {
      if (error != NULL)
        {
          fprintf (stderr, "Call failed because %s.\n", error->message);
        } // if we got an error
      else
        {
          fprintf (stderr, "Call failed for an unknown reason.\n");
        }
      return 1; // Give up!
    } // if no value was result
  
  return g_variant_to_scheme_result(gresult);
} // loudbus_services


static Scheme_Object *
loudbus_objects (int argc, Scheme_Object **argv)
{
  LouDBusProxy *proxy;
  GError *error;
  GVariant *gresult, *params;
  gchar *service;

  service = scheme_object_to_string(argv[0]);
  
  proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         NULL,
                                         service,
                                         "",
                                         "",
                                         NULL,
                                         &error);

  params = g_variant_new("()");

 gresult = g_dbus_proxy_call_sync (proxy,
                                    "",
                                    params,
                                    0,
                                    -1,
                                    NULL,
                                    &error);

  return g_variant_to_scheme_result(gresult);
}



// +-----------------------+------------------------------------------
// | Standard Scheme Setup |
// +-----------------------+

Scheme_Object *
scheme_reload (Scheme_Env *env)
{
  Scheme_Env *menv = NULL;      // The module's environment
  Scheme_Object *proc = NULL;   // A Procedure we're adding

  // Annotations for the garbage collector
  MZ_GC_DECL_REG (2);
  MZ_GC_VAR_IN_REG (0, env);
  MZ_GC_VAR_IN_REG (1, menv);
  MZ_GC_REG ();

  // Build the module environment.
  menv = scheme_primitive_module (scheme_intern_symbol ("loudbus"),
                                  env);

  // Build the procedures
  proc = scheme_make_prim_w_arity (loudbus_call, "loudbus-call", 2, -1);
  scheme_add_global ("loudbus-call", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_import, "loudbus-import", 3, 3),
    scheme_add_global ("loudbus-import", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_init, "loudbus-init", 1, 1),
    scheme_add_global ("loudbus-init", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_methods, "loudbus-methods", 1, 1),
    scheme_add_global ("loudbus-methods", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_proxy, "loudbus-proxy", 3, 3),
    scheme_add_global ("loudbus-proxy", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_method_info, "loudbus-method-info",
  				   2, 2),
    scheme_add_global ("loudbus-method-info", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_services, "loudbus-services", 0, 0),
    scheme_add_global ("loudbus-services", proc, menv);

  proc = scheme_make_prim_w_arity (loudbus_objects, "loudbus-objects", 1, 1),
    scheme_add_global ("loudbus-objects", proc, menv);


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

  // We're using GLib, so we should start with this lovely function.
  g_type_init ();

  return scheme_reload (env);
} // scheme_initialize

Scheme_Object *
scheme_module_name (void)
{
  return scheme_intern_symbol ("loudbus");
} // scheme_module_name
