  /*
Prettier, but unwanted output:
  result = scheme_make_pair (scheme_make_locale_string ("Annotation(s):"), result);

  //Add parameters onto the list.
  for (m = g_dbus_method_info_num_formals (method) - 1; m >= 0; m--)
    {
      param = method->in_args[m];
      val2 = scheme_make_locale_string (param->name); //Holds the parameter's name
      result = scheme_make_pair (val2, result);
      val3 = scheme_make_locale_string (param->signature); //Holds the parameter's type
      result = scheme_make_pair (val3, result);
    }

  result = scheme_make_pair (scheme_make_locale_string ("Parameters (name followed by type):"), result);
  */
