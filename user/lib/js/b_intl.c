/*
 * Intl (27s): the ECMAScript internationalization API for English
 * (en-US; other locales are taken and fall back to it). It is written in
 * JavaScript (intl.c's intl_source) and compiled the first time a script
 * touches the global Intl or a toLocaleString, toLocaleDateString,
 * toLocaleTimeString or localeCompare: worlds that never do pay nothing.
 * Until then the global Intl is an accessor that loads it and puts the
 * real object in its place.
 */
#include "jsint.h"

extern const char intl_source[];

/* the object the source returns: { Intl, numberToLocale, dateToLocale,
 * localeCompare } (JS_EXCEPTION if it threw) */
static jsval intl_api(struct js *J)
{
    if (js_is_object(J->intl))
        return J->intl;
    jsval f = js_eval(J, intl_source, strlen(intl_source), "intl");
    if (f == JS_EXCEPTION)
        return f;
    jsval api = vm_call(J, f, JS_UNDEFINED, 0, 0, JS_UNDEFINED);
    if (api == JS_EXCEPTION)
        return api;
    J->intl = api;
    return api;
}

static jsval intl_get(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)argv, (void)nt;
    jsval api = intl_api(J);
    if (api == JS_EXCEPTION)
        return api;
    jsval intl = js_get(J, api, "Intl");
    if (intl != JS_EXCEPTION)
        js_define_value(J, obj_val(J->global), "Intl", intl);
    return intl;
}

static jsval intl_set(struct js *J, jsval this_val, int argc, jsval *argv, jsval nt)
{
    (void)this_val, (void)argc, (void)nt;
    js_define_value(J, obj_val(J->global), "Intl", argv[0]);
    return JS_UNDEFINED;
}

/* calls the api's function `name` */
static jsval intl_call(struct js *J, const char *name, int argc, jsval *argv)
{
    jsval api = intl_api(J);
    if (api == JS_EXCEPTION)
        return api;
    jsval f = js_get(J, api, name);
    if (f == JS_EXCEPTION)
        return f;
    return vm_call(J, f, JS_UNDEFINED, argc, argv, JS_UNDEFINED);
}

jsval intl_number_to_locale(struct js *J, jsval x, jsval locales, jsval options)
{
    jsval a[3] = { x, locales, options };
    return intl_call(J, "numberToLocale", 3, a);
}

jsval intl_date_to_locale(struct js *J, double t, jsval locales, jsval options, const char *required, const char *defaults)
{
    jsval a[5] = { js_number(t), locales, options, js_cstring(J, required), js_cstring(J, defaults) };
    return intl_call(J, "dateToLocale", 5, a);
}

jsval intl_locale_compare(struct js *J, jsval a, jsval b, jsval locales, jsval options)
{
    jsval v[4] = { a, b, locales, options };
    return intl_call(J, "localeCompare", 4, v);
}

void init_intl(struct js *J)
{
    J->intl = JS_UNDEFINED;
    jsval key = str_val(atom(J, str_from_utf8(J, "Intl", 4)));
    obj_define_accessor(J, J->global, key, obj_val(make_native(J, "get Intl", intl_get, 0, 0)),
                        obj_val(make_native(J, "set Intl", intl_set, 1, 0)), PROP_CONFIGURABLE);
}
