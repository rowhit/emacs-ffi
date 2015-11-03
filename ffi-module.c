
#include "emacs_module.h"

#include <ffi.h>
#include <ltdl.h>
#include <string.h>
#include <assert.h>

int plugin_is_GPL_compatible;

static emacs_value nil;
static emacs_value emacs_true;
static emacs_value wrong_type_argument;
static emacs_value error;

#define ARRAY_SIZE(x) (sizeof (x) / sizeof (x[0]))

union holder
{
  int8_t i8;
  uint8_t ui8;
  int16_t i16;
  uint16_t ui16;
  int32_t i32;
  uint32_t ui32;
  int64_t i64;
  uint64_t ui64;
  float f;
  double d;
  unsigned char uc;
  signed char c;
  short s;
  unsigned short us;
  int i;
  unsigned int ui;
  long l;
  unsigned long ul;
  void *p;
};

struct type_descriptor
{
  const char *name;
  ffi_type *type;
  emacs_value value;
};

static struct type_descriptor type_descriptors[] =
{
  { ":void", &ffi_type_void },
  { ":int8", &ffi_type_sint8 },
  { ":uint8", &ffi_type_uint8 },
  { ":int16", &ffi_type_sint16 },
  { ":uint16", &ffi_type_uint16 },
  { ":int32", &ffi_type_sint32 },
  { ":uint32", &ffi_type_uint32 },
  { ":int64", &ffi_type_sint64 },
  { ":uint64", &ffi_type_uint64 },
  { ":float", &ffi_type_float },
  { ":double", &ffi_type_double },
  { ":uchar", &ffi_type_uchar },
  { ":char", &ffi_type_schar },
  { ":ushort", &ffi_type_ushort },
  { ":short", &ffi_type_sshort },
  { ":uint", &ffi_type_uint },
  { ":int", &ffi_type_sint },
  { ":ulong", &ffi_type_ulong },
  { ":long", &ffi_type_slong },
  { ":pointer", &ffi_type_pointer },

  { ":size_t", NULL },
  { ":ssize_t", NULL },
  { ":bool", NULL }
};

// Description of a closure, freed by free_closure_desc.
struct closure_description
{
  emacs_env *env;
  void *closure;
  void *code;
  emacs_value cif_ref;
  emacs_value func_ref;
};

static void free_closure_desc (void *);
static void free_type (void *);



static void
null_finalizer (void *ptr)
{
}

// We have multiple finalizers because we use them to distinguish
// between pointer types.
static void
null_lthandle_finalizer (void *ptr)
{
}

static void *
unwrap_pointer (emacs_env *env, emacs_value value,
		emacs_finalizer_function expected)
{
  emacs_finalizer_function finalizer
    = env->get_user_ptr_finalizer (env, value);
  if (finalizer == NULL)
    return NULL;
  if (finalizer != expected)
    {
      env->error_signal (env, wrong_type_argument, value);
      return NULL;
    }
  return env->get_user_ptr_ptr (env, value);
}



/* (ffi--dlopen str) */
static emacs_value
ffi_dlopen (emacs_env *env, int nargs, emacs_value args[], void *ignore)
{
  lt_dlhandle handle;

  size_t length = 0;
  env->copy_string_contents (env, args[0], NULL, &length);
  char *name = malloc (length);
  env->copy_string_contents (env, args[0], name, &length);

  handle = lt_dlopenext (name);
  free (name);

  if (!handle)
    {
      // FIXME lt_dlerror
      // FIXME use NAME here in the error
      env->error_signal (env, error, nil);
      return NULL;
    }
  return env->make_user_ptr (env, null_lthandle_finalizer, handle);
}

/* (ffi--dlsym symbol-name handle) */
static emacs_value
ffi_dlsym (emacs_env *env, int nargs, emacs_value args[], void *ignore)
{
  lt_dlhandle handle = unwrap_pointer (env, args[1], null_lthandle_finalizer);
  if (!handle)
    return NULL;

  size_t length = 0;
  env->copy_string_contents (env, args[0], NULL, &length);
  char *name = malloc (length);
  env->copy_string_contents (env, args[0], name, &length);

  void *sym = lt_dlsym (handle, name);
  free (name);

  if (sym == NULL)
    return nil;
  return env->make_user_ptr (env, null_finalizer, sym);
}

static ffi_type *
convert_type_from_lisp (emacs_env *env, emacs_value ev_type)
{
  emacs_value type = ev_type;
  unsigned int i;
  for (i = 0; i < ARRAY_SIZE (type_descriptors); ++i)
    if (env->eq (env, type, type_descriptors[i].value))
      return type_descriptors[i].type;

  // If we have an ffi_type object, just unwrap it.
  emacs_finalizer_function finalizer
    = env->get_user_ptr_finalizer (env, ev_type);
  if (env->error_check (env))
    {
      // We're going to set our own error.
      env->error_clear (env);
    }
  else if (finalizer == free_type)
    return env->get_user_ptr_ptr (env, ev_type);

  env->error_signal (env, wrong_type_argument, ev_type);
  return NULL;
}

static void
free_cif (void *p)
{
  ffi_cif *cif = p;
  free (cif->arg_types);
  free (cif);
}

/* (ffi--prep-cif return-type arg-types &optional n-fixed-args) */
static emacs_value
module_ffi_prep_cif (emacs_env *env, int nargs, emacs_value args[],
		     void *ignore)
{
  unsigned int i;
  ffi_type *return_type;
  size_t n_types;
  ffi_type **arg_types;
  emacs_value typevec = args[1];
  emacs_value result = NULL;

  return_type = convert_type_from_lisp (env, args[0]);
  if (!return_type)
    return NULL;

  n_types = env->vec_size (env, typevec);
  if (env->error_check (env))
    return NULL;
  arg_types = malloc (n_types * sizeof (ffi_type *));

  for (i = 0; i < n_types; ++i)
    {
      emacs_value this_type = env->vec_get (env, typevec, i);
      if (!this_type)
	goto fail;
      arg_types[i] = convert_type_from_lisp (env, this_type);
      if (!arg_types[i])
	goto fail;
    }

  ffi_cif *cif = malloc (sizeof (ffi_cif));
  ffi_status status;
  if (nargs == 3)
    {
      int64_t n_fixed = env->fixnum_to_int (env, args[2]);
      if (env->error_check (env))
	goto fail;
      status = ffi_prep_cif_var (cif, FFI_DEFAULT_ABI, n_fixed,
				 n_types, return_type, arg_types);
    }
  else
    status = ffi_prep_cif (cif, FFI_DEFAULT_ABI, n_types,
			   return_type, arg_types);

  if (status)
    /* FIXME add some useful message */
    env->error_signal (env, error, nil);
  else
    result = env->make_user_ptr (env, free_cif, cif);

  if (!result)
    free_cif (cif);
 fail:
  return result;
}

static bool
convert_from_lisp (emacs_env *env, ffi_type *type, emacs_value ev,
		   union holder *result)
{
  // Callers must handle this.
  assert (type->type != FFI_TYPE_STRUCT);

#define MAYBE_NUMBER(ftype, field)			\
  else if (type == &ffi_type_ ## ftype)			\
    {							\
    int64_t ival = env->fixnum_to_int (env, ev);	\
    if (env->error_check (env))				\
      return false;					\
  result->field = ival;					\
    }

  if (type == &ffi_type_void)
    {
      env->error_signal (env, wrong_type_argument, ev);
      return false;
    }
  MAYBE_NUMBER (sint8, i8)
  MAYBE_NUMBER (uint8, ui8)
  MAYBE_NUMBER (sint16, i16)
  MAYBE_NUMBER (uint16, ui16)
  MAYBE_NUMBER (sint32, i32)
  MAYBE_NUMBER (uint32, ui32)
  MAYBE_NUMBER (sint64, i64)
  MAYBE_NUMBER (uint64, ui64)
  MAYBE_NUMBER (schar, c)
  MAYBE_NUMBER (uchar, uc)
  MAYBE_NUMBER (sint, i)
  MAYBE_NUMBER (uint, ui)
  MAYBE_NUMBER (sshort, s)
  MAYBE_NUMBER (ushort, us)
  MAYBE_NUMBER (slong, l)
  MAYBE_NUMBER (ulong, ul)
  else if (type == &ffi_type_float)
    {
      double d = env->float_to_c_double (env, ev);
      if (env->error_check (env))
	return false;
      result->f = d;
    }
  else if (type == &ffi_type_double)
    {
      double d = env->float_to_c_double (env, ev);
      if (env->error_check (env))
	return false;
      result->d = d;
    }
  else if (type == &ffi_type_pointer)
    {
      void *p = env->get_user_ptr_ptr (env, ev);
      // We use the finalizer to detect whether we have a closure
      // pointer; these are converted to their code pointer, not their
      // raw pointer.
      if (env->error_check (env))
	return false;
 
      emacs_finalizer_function finalizer
	= env->get_user_ptr_finalizer (env, ev);
      if (env->error_check (env))
	return false;

      if (finalizer == free_closure_desc)
	{
	  struct closure_description *desc = p;
	  p = desc->code;
	}

      result->p = p;
    }
  else
    {
      env->error_signal (env, wrong_type_argument, ev);
      return false;
    }

#undef MAYBE_NUMBER

return true;
}

static emacs_value
convert_to_lisp (emacs_env *env, ffi_type *type, union holder *value,
		 emacs_finalizer_function finalizer_for_struct)
{
  emacs_value result;

#define MAYBE_NUMBER(ftype, field)		\
  else if (type == &ffi_type_ ## ftype)		\
    result = env->make_fixnum (env, value->field);

  if (type == &ffi_type_void)
    result = nil;
  MAYBE_NUMBER (sint8, i8)
  MAYBE_NUMBER (uint8, ui8)
  MAYBE_NUMBER (sint16, i16)
  MAYBE_NUMBER (uint16, ui16)
  MAYBE_NUMBER (sint32, i32)
  MAYBE_NUMBER (uint32, ui32)
  MAYBE_NUMBER (sint64, i64)
  MAYBE_NUMBER (uint64, ui64)
  MAYBE_NUMBER (schar, c)
  MAYBE_NUMBER (uchar, uc)
  MAYBE_NUMBER (sint, i)
  MAYBE_NUMBER (uint, ui)
  MAYBE_NUMBER (sshort, s)
  MAYBE_NUMBER (ushort, us)
  MAYBE_NUMBER (slong, l)
  MAYBE_NUMBER (ulong, ul)
  else if (type == &ffi_type_float)
    result = env->make_float (env, value->f);
  else if (type == &ffi_type_double)
    result = env->make_float (env, value->d);
  else if (type == &ffi_type_pointer)
    result = env->make_user_ptr (env, null_finalizer, value->p);
  else if (type->type == FFI_TYPE_STRUCT)
    {
      // The argument itself is the data.
      result = env->make_user_ptr (env, null_finalizer, value);
    }
  else
    {
      env->error_signal (env, wrong_type_argument, nil);
      result = NULL;
    }

#undef MAYBE_NUMBER

  return result;
}

/* (ffi--call cif function &rest args) */
static emacs_value
module_ffi_call (emacs_env *env, int nargs, emacs_value *args, void *ignore)
{
  ffi_cif *cif = unwrap_pointer (env, args[0], free_cif);
  if (!cif)
    return NULL;

  void *fn = env->get_user_ptr_ptr (env, args[1]);
  if (env->error_check (env))
    return NULL;

  void **values;
  union holder *holders;
  union holder result_holder;
  void *result;
  emacs_value lisp_result = NULL;

  nargs -= 2;
  args += 2;

  if (nargs == 0)
    {
      values = NULL;
      holders = NULL;
    }
  else
    {
      holders = malloc (nargs * sizeof (union holder));
      values = malloc (nargs * sizeof (void *));

      unsigned int i;
      for (i = 0; i < nargs; ++i)
	{
	  if (cif->arg_types[i]->type == FFI_TYPE_STRUCT)
	    {
	      // The value is just the unwrapped pointer.
	      values[i] = env->get_user_ptr_ptr (env, args[i]);
	      if (!values[i])
		goto fail;
	    }
	  else
	    {
	      if (!convert_from_lisp (env, cif->arg_types[i], args[i],
				      &holders[i]))
		goto fail;
	      values[i] = &holders[i];
	    }
	}
    }

  if (cif->rtype->type == FFI_TYPE_STRUCT)
    result = malloc (cif->rtype->size);
  else
    result = &result_holder;

  ffi_call (cif, fn, result, values);

  lisp_result = convert_to_lisp (env, cif->rtype, result, free);
  if (lisp_result)
    {
      // On success do not free RESULT.
      result = &result_holder;
    }

 fail:
  if (result != &result_holder)
    free (result);
  free (values);
  free (holders);

  return lisp_result;
}

/* (ffi--mem-ref POINTER TYPE) */
static emacs_value
module_ffi_mem_ref (emacs_env *env, int nargs, emacs_value *args, void *ignore)
{
  void *ptr = env->get_user_ptr_ptr (env, args[0]);
  if (env->error_check (env))
    return NULL;

  ffi_type *type = convert_type_from_lisp (env, args[1]);
  if (!type)
    return NULL;

  return convert_to_lisp (env, type, ptr, null_finalizer);
}

/* (ffi--mem-set POINTER TYPE VALUE) */
static emacs_value
module_ffi_mem_set (emacs_env *env, int nargs, emacs_value *args, void *ignore)
{
  void *ptr = env->get_user_ptr_ptr (env, args[0]);
  if (env->error_check (env))
    return NULL;

  ffi_type *type = convert_type_from_lisp (env, args[1]);
  if (!type)
    return NULL;

  if (type->type == FFI_TYPE_STRUCT)
    {
      void *from = env->get_user_ptr_ptr (env, args[2]);
      if (env->error_check (env))
	return NULL;
      memcpy (ptr, from, type->size);
    }
  else if (!convert_from_lisp (env, type, args[2], ptr))
    return NULL;

  return nil;
}

/* (ffi-pointer+ POINTER NUM) */
static emacs_value
module_ffi_pointer_plus (emacs_env *env, int nargs, emacs_value *args,
			 void *ignore)
{
  char *ptr = env->get_user_ptr_ptr (env, args[0]);
  if (env->error_check (env))
    return NULL;
  ptr += env->fixnum_to_int (env, args[1]);
  if (env->error_check (env))
    return NULL;
  return env->make_user_ptr (env, null_finalizer, ptr);
}

/* (ffi-pointer-null-p POINTER) */
static emacs_value
module_ffi_pointer_null_p (emacs_env *env, int nargs, emacs_value *args,
			   void *ignore)
{
  void *ptr = env->get_user_ptr_ptr (env, args[0]);
  if (env->error_check (env))
    {
      env->error_clear (env);
      return nil;
    }
  return ptr ? emacs_true : nil;
}

/* (ffi-get-c-string POINTER) */
static emacs_value
module_ffi_get_c_string (emacs_env *env, int nargs, emacs_value *args,
			 void *ignore)
{
  char *ptr = env->get_user_ptr_ptr (env, args[0]);
  if (env->error_check (env))
    return NULL;
  size_t len = strlen (ptr);
  return env->make_string (env, ptr, len);
}



static void
generic_callback (ffi_cif *cif, void *ret, void **args, void *d)
{
  struct closure_description *desc = d;
  emacs_env *env = desc->env;

  emacs_value *argvalues = malloc (cif->nargs * sizeof (emacs_value));
  int i;
  for (i = 0; i < cif->nargs; ++i)
    {
      argvalues[i] = convert_to_lisp (env, cif->arg_types[i], args[i],
				      null_finalizer);
      if (!argvalues[i])
	goto fail;
    }

  emacs_value value = env->funcall (env, desc->func_ref, cif->nargs, argvalues);
  if (value)
    {
      if (cif->rtype->type == FFI_TYPE_STRUCT)
	{
	  void *ptr = env->get_user_ptr_ptr (env, value);
	  if (!env->error_check (env))
	    memcpy (ret, ptr, cif->rtype->size);
	}
      else
	convert_from_lisp (env, cif->rtype, value, ret);
    }

  env->error_clear (env);

 fail:
  // On failure we might leave the result uninitialized, but there's
  // nothing to do about it.
  free (argvalues);
}

static void
free_closure_desc (void *d)
{
  struct closure_description *desc = d;

  desc->env->free_global_ref (desc->env, desc->cif_ref);
  desc->env->free_global_ref (desc->env, desc->func_ref);
  ffi_closure_free (desc->closure);
}

/* (ffi-make-closure CIF FUNCTION) */
static emacs_value
module_ffi_make_closure (emacs_env *env, int nargs, emacs_value *args,
			 void *ignore)
{
  emacs_value cif_ref = NULL;
  emacs_value func = NULL;
  struct closure_description *desc = NULL;

  cif_ref = env->make_global_ref (env, args[0]);
  if (!cif_ref)
    return NULL;
  func = env->make_global_ref (env, args[1]);
  if (!func)
    goto fail;

  ffi_cif *cif = unwrap_pointer (env, args[0], free_cif);
  if (!cif)
    goto fail;

  void *code;
  void *writable = ffi_closure_alloc (sizeof (ffi_closure), &code);

  desc = malloc (sizeof (struct closure_description));
  desc->env = env;
  desc->closure = writable;
  desc->code = code;
  desc->cif_ref = cif_ref;
  desc->func_ref = func;

  ffi_status status = ffi_prep_closure_loc (writable, cif, generic_callback,
					    desc, code);
  if (status)
    /* FIXME add some useful message */
    env->error_signal (env, error, nil);
  else
    {
      emacs_value desc_val = env->make_user_ptr (env, free_closure_desc, desc);
      if (desc_val)
	return desc_val;
    }

 fail:
  free (desc);
  if (func)
    env->free_global_ref (env, func);
  env->free_global_ref (env, cif_ref);
  return NULL;
}



/* (ffi--type-size TYPE) */
static emacs_value
module_ffi_type_size (emacs_env *env, int nargs, emacs_value *args,
		      void *ignore)
{
  ffi_type *type = convert_type_from_lisp (env, args[0]);
  if (!type)
    return NULL;
  return env->make_fixnum (env, type->size);
}

/* (ffi--type-alignment TYPE) */
static emacs_value
module_ffi_type_alignment (emacs_env *env, int nargs, emacs_value *args,
			   void *ignore)
{
  ffi_type *type = convert_type_from_lisp (env, args[0]);
  if (!type)
    return NULL;
  return env->make_fixnum (env, type->alignment);
}



static void
free_type (void *t)
{
  ffi_type *type = t;
  free (type->elements);
  free (type);
}

/* (ffi--define-struct &rest TYPES) */
static emacs_value
module_ffi_define_struct (emacs_env *env, int nargs, emacs_value *args,
			  void *ignore)
{
  emacs_value result = NULL;
  ffi_type *type = malloc (sizeof (ffi_type));

  type->size = 0;
  type->alignment = 0;
  type->type = FFI_TYPE_STRUCT;
  type->elements = malloc ((nargs + 1) * sizeof (struct ffi_type *));
  type->elements[nargs] = NULL;

  int i;
  for (i = 0; i < nargs; ++i)
    {
      type->elements[i] = convert_type_from_lisp (env, args[i]);
      if (!type->elements[i])
	goto fail;
    }

  // libffi will fill in the type for us, but only if we use prep_cif.
  // It would be great if it did more.
  ffi_cif temp_cif;
  if (ffi_prep_cif (&temp_cif, FFI_DEFAULT_ABI, 0, type, NULL) != FFI_OK)
    {
      /* FIXME add some useful message */
      env->error_signal (env, error, nil);
      goto fail;
    }

  result = env->make_user_ptr (env, free_type, type);
  if (result)
    return result;

 fail:
  free_type (type);
  return result;
}



struct descriptor
{
  const char *name;
  int min, max;
  emacs_subr subr;
};

static const struct descriptor exports[] =
{
  { "ffi--dlopen", 1, 1, ffi_dlopen },
  { "ffi--dlsym", 2, 2, ffi_dlsym },
  { "ffi--prep-cif", 1, emacs_variadic_function, module_ffi_prep_cif },
  { "ffi--call", 2, emacs_variadic_function, module_ffi_call },
  { "ffi--mem-ref", 2, 2, module_ffi_mem_ref },
  { "ffi--mem-set", 3, 3, module_ffi_mem_set },
  { "ffi-pointer+", 2, 2, module_ffi_pointer_plus },
  { "ffi-pointer-null-p", 1, 1, module_ffi_pointer_null_p },
  { "ffi-get-c-string", 1, 1, module_ffi_get_c_string },
  { "ffi-make-closure", 2, 2, module_ffi_make_closure },
  { "ffi--type-size", 1, 1, module_ffi_type_size },
  { "ffi--type-alignment", 1, 1, module_ffi_type_alignment },
  { "ffi--define-struct", 1, emacs_variadic_function,
    module_ffi_define_struct }
};

static bool
get_global (emacs_env *env, emacs_value *valptr, const char *name)
{
  *valptr = env->intern (env, name);
  if (!*valptr)
    return false;
  *valptr = env->make_global_ref (env, *valptr);
  return *valptr != NULL;
}

static void
init_type_alias (const char *name, bool is_unsigned, int size)
{
  int i;
  // Start at the end since we know it is a bit more efficient.
  for (i = ARRAY_SIZE (type_descriptors) - 1; i >= 0; --i)
    {
      if (!strcmp (type_descriptors[i].name, name))
	break;
    }
  assert (i >= 0);

  ffi_type **type = &type_descriptors[i].type;
  switch (size)
    {
    case 1:
      *type = is_unsigned ? &ffi_type_sint8 : &ffi_type_uint8;
      break;
    case 2:
      *type = is_unsigned ? &ffi_type_sint16 : &ffi_type_uint16;
      break;
    case 4:
      *type = is_unsigned ? &ffi_type_sint32 : &ffi_type_uint32;
      break;
    case 8:
      *type = is_unsigned ? &ffi_type_sint64 : &ffi_type_uint64;
      break;
    default:
      abort ();
    }
}

int
emacs_module_init (struct emacs_runtime *runtime)
{
  unsigned int i;
  emacs_env *env = runtime->get_environment (runtime);

  lt_dlinit ();

  init_type_alias (":size_t", true, sizeof (size_t));
  init_type_alias (":ssize_t", false, sizeof (ssize_t));
  init_type_alias (":bool", true, sizeof (bool));

  if (!get_global (env, &nil, "nil")
      || !get_global (env, &emacs_true, "t")
      || !get_global (env, &wrong_type_argument, "wrong-type-argument")
      || !get_global (env, &error, "error"))
    return -1;

  emacs_value fset = env->intern (env, "fset");

  for (i = 0; i < ARRAY_SIZE (type_descriptors); ++i)
    {
      if (!get_global (env, &type_descriptors[i].value,
		       type_descriptors[i].name))
	return -1;
    }

  for (i = 0; i < ARRAY_SIZE (exports); ++i)
    {
      emacs_value func = env->make_function (env, exports[i].min,
					     exports[i].max, exports[i].subr,
					     NULL);
      if (!func)
	return -1;
      emacs_value sym = env->intern (env, exports[i].name);
      if (!sym)
	return -1;

      emacs_value args[2] = { sym, func };
      if (!env->funcall (env, fset, 2, args))
	return -1;
    }

  return 0;
}
