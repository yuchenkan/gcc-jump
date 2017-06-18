#include <stdio.h>
#include <assert.h>
#include <stdarg.h>

#include <string>

#include "gcc-plugin.h"
#include "plugin-version.h"
#include "output.h"
#include "toplev.h"
#include "opts.h"
#include "tree.h"
#include "print-tree.h"
#include "c-family/c-common.h"
#include "cpplib.h"
#include "wide-int-print.h"
#include "real.h"

#include "gcj.hpp"

int plugin_is_GPL_compatible;

static gcj::file_location
build_file_location (source_location l)
{
  return gcj::file_location (LOCATION_LINE (l), LOCATION_COLUMN (l));
}

static gcj::source_location
build_source_location (gcj::unit *unit, source_location l)
{
  return gcj::source_location (unit->file_id (LOCATION_FILE (l)),
			       build_file_location (l));
}

static gcj::source_location
build_source_location (gcj::unit *unit, const line_map_ordinary *m)
{
  return gcj::source_location (unit->file_id (LINEMAP_FILE (m)),
			       gcj::file_location (LAST_SOURCE_LINE (m),
						   0));
}

static void
cb_start_unit (void *, void *data)
{
  gcj::set *set = (gcj::set *) data;

  int cp = 0;
  std::map<unsigned int, int> priorities;
  priorities.insert (std::make_pair (OPT_SPECIAL_input_file, cp ++));
  priorities.insert (std::make_pair (OPT_D, cp ++));
  priorities.insert (std::make_pair (OPT_I, cp ++));

  std::string input;
  std::map<int, std::string> arg_map;

  for (size_t i = 1; i < save_decoded_options_count; ++ i)
    {
      unsigned int opt_index;
      opt_index = save_decoded_options[i].opt_index;
      set->trace ("option: %lu %s %s",
		  opt_index,
		  save_decoded_options[i].arg,
		  save_decoded_options[i].orig_option_with_args_text);

      if (opt_index == OPT_SPECIAL_input_file)
	input = save_decoded_options[i].arg;

      int p;
      p = priorities.find (opt_index) == priorities.end()
	  ? 255 : priorities.find (opt_index)->second;

      if (arg_map.find (p) == arg_map.end ())
	arg_map.insert (std::make_pair (p, std::string ()));

      std::string *a = &arg_map.find (p)->second;
      for (size_t j = 0; j < save_decoded_options[i].canonical_option_num_elements; ++ j)
	{
	  set->trace ("\n  %s",
		      save_decoded_options[i].canonical_option[j]);

	  if (! a->empty ()) *a += ' ';
	  *a += escape (save_decoded_options[i].canonical_option[j], ' ');
	}
      set->trace ("\n");
    }

  std::string args;
  std::map<int, std::string>::iterator it;
  for (it = arg_map.begin (); it != arg_map.end (); ++ it)
    {
      if (! args.empty ()) args += ' ';
      args += it->second;
    }

  set->next (args, input);

  if (! flag_syntax_only)
    {
      section * sec;
      sec = get_section (".GCJ.plugin",
			 SECTION_DEBUG
			 | SECTION_MERGE
			 | SECTION_STRINGS
			 | (SECTION_ENTSIZE & 1),
			 NULL);
      switch_to_section (sec);

      ASM_OUTPUT_ASCII (asm_out_file, args.c_str(), args.size());
      ASM_OUTPUT_SKIP (asm_out_file, (unsigned HOST_WIDE_INT) 1);
    }
}

static void
unwind_include (gcj::unit *unit, source_location loc,
		gcj::source_stack *stack, const char *prefix)
{
  assert (loc != UNKNOWN_LOCATION);

  int fid = unit->file_id (LOCATION_FILE (loc));
  stack->add (gcj::source_location (fid));

  const line_map_ordinary *m;
  linemap_resolve_location (line_table, loc,
			    LRK_MACRO_EXPANSION_POINT, &m);

  while (m && ! MAIN_FILE_P (m))
    {
      m = INCLUDED_FROM (line_table, m);
      unit->trace ("  %s, included from %s:%d\n",
		   prefix,
		   LINEMAP_FILE (m), LAST_SOURCE_LINE (m));

      stack->add (build_source_location (unit, m));
    }
}

static void
unwind_macro (gcj::unit *unit, source_location loc,
	      gcj::macro_stack *stack, const char *prefix)
{
  if (loc <= BUILTINS_LOCATION)
    return;

  source_location w = loc;
  const line_map *m;
  for (m = linemap_lookup (line_table, w);
       linemap_macro_expansion_map_p (m);
       w = linemap_unwind_toward_expansion (line_table, w, &m))
    {
      source_location l =
	linemap_resolve_location (line_table, w,
				  LRK_MACRO_DEFINITION_LOCATION,
				  NULL);
      unit->trace ("  %s, expanded from %s:%d,%d\n",
		   prefix,
		   LOCATION_FILE (l), LOCATION_LINE (l),
		   LOCATION_COLUMN (l));

      gcj::source_stack include;
      unwind_include (unit, l, &include, prefix);

      stack->add (gcj::expansion_point (unit->include_id (include),
					build_source_location (unit, l)));
    }
}

static void
unwind (gcj::unit *unit, source_location loc,
	gcj::unwind_stack *stack, const char *prefix)
{
  unwind_macro (unit, loc, &stack->macro, prefix);
  unwind_include (unit, loc, &stack->include, prefix);
}

static std::string
spell_integer (tree value)
{
  char buf[WIDE_INT_PRINT_BUFFER_SIZE];
  print_dec (value, buf, TYPE_SIGN (TREE_TYPE (value)));
  return buf;
}

static std::string
spell_real (tree value)
{
  REAL_VALUE_TYPE d;
  d = TREE_REAL_CST (value);
  char buf[60];
  real_to_decimal (buf, &d, sizeof (buf), 0, 1);
  return buf;
}

#define OP(e, s) s,
#define TK(e, s) #e,
static const char *spellings[N_TTYPES] = { TTYPE_TABLE };
#undef OP
#undef TK

static std::string
spell (const cpp_token_arg *token)
{
  if (token->value != NULL_TREE
      && token->type != CPP_PRAGMA)
    {
      tree value = token->value;
      if (TREE_CODE (value) == USERDEF_LITERAL)
	value = USERDEF_LITERAL_VALUE (value);
      tree_code code = TREE_CODE (value);

      if (code == IDENTIFIER_NODE)
	return IDENTIFIER_POINTER (value);
      else if (code == INTEGER_CST)
	return spell_integer (value);
      else if (code == REAL_CST)
	return spell_real (value);
      else if (code == COMPLEX_CST)
	{
	  if (TREE_CODE (TREE_IMAGPART (value)) == INTEGER_CST)
	    return spell_integer (TREE_IMAGPART (value)) + 'i';
	  else if (TREE_CODE (TREE_IMAGPART (value)) == REAL_CST)
	    return spell_real (TREE_IMAGPART (value)) + 'i';
	}
      else if (code == STRING_CST)
	{
	  std::string str;
	  const char *p;
	  for (p = TREE_STRING_POINTER (value); *p; ++ p)
	    {
	      if (*p == '\n') str += "\\n";
	      else if (*p == '\t') str += "\\t";
	      else if (*p == '\r') str += "\\r";
	      else if (*p == '\\') str += "\\\\";
	      else if (*p == '\"') str += "\\\"";
	      else str += *p;
	    }
	  return '"' + str + '"';
	}
    }
  return spellings[token->type];
}

static void
cb_cpp_token (void *arg, void  *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current ();

  if (! unit)
    return;

  source_location loc = ((cpp_token_arg *) arg)->loc;

  gcj::unwind_stack stack;
  unwind_macro (unit, loc, &stack.macro, "cpp_token");

  if (stack.macro.length () == 0)
    return;

  set->trace ("cpp_token %s at %s:%d,%d\n",
	      spell ((cpp_token_arg *) arg).c_str (),
	      LOCATION_FILE (loc), LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc));

  unwind_include (unit, loc, &stack.include, "cpp_token");

  gcj::context *ctx = unit->get (unit->include_id (stack.include));
  gcj::jump_to *to = ctx->jump (unit, build_file_location (loc), 0);
  assert (to && to->exp);
  gcj::source_stack exp (stack.macro);
  std::string token = spell ((cpp_token_arg *) arg);
  unit->get_expansion (to->exp)->add (token, exp);
}

static void
build_ref_jump_from (const gcj::file_location& loc,
		     int len,
		     const gcj::unit *unit,
		     gcj::context *ctx, const gcj::macro_stack& stack,
		     gcj::jump_from *from)
{
  if (stack.length () == 0)
    *from = gcj::jump_from (loc, len);
  else
    {
      gcj::jump_to *jump_to = ctx->jump (unit, loc, 0);
      assert (jump_to && jump_to->exp);
      gcj::source_stack exp (stack);
      int id = unit->get_expansion (jump_to->exp)->id (exp);
      *from = gcj::jump_from (loc, 0, id);
    }
}

static void
build_ref_jump_to (int include,
		   const gcj::file_location& loc,
		   const gcj::unit *unit,
		   const gcj::macro_stack& stack,
		   gcj::jump_to *to)
{
  if (stack.length () == 0)
    *to = gcj::jump_to (include, 0, loc);
  else
    {
      const gcj::context *ctx = unit->get (include);
      const gcj::jump_to *jump_to = ctx->jump (unit, loc, 0);
      assert (jump_to && jump_to->exp);
      const gcj::expansion *exp = unit->get_expansion (jump_to->exp);
      int id = exp->id (gcj::source_stack (stack));
      *to = gcj::jump_to (include, 0, loc, id);
    }
}

static void
cb_external_ref (void *arg, void *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current();

  if (! unit)
    return;

  tree decl = ((external_ref_arg *) arg)->decl;
  source_location from_loc = ((external_ref_arg *) arg)->loc;
  source_location to_loc = DECL_SOURCE_LOCATION (decl);
  set->trace ("build_external_ref %s declared at %s:%d,%d\n",
	      IDENTIFIER_POINTER (DECL_NAME (decl)),
	      LOCATION_FILE (to_loc),
	      LOCATION_LINE (to_loc),
	      LOCATION_COLUMN (to_loc));
  
  set->trace ("refered by %s:%d,%d\n",
	      LOCATION_FILE (from_loc),
	      LOCATION_LINE (from_loc),
	      LOCATION_COLUMN (from_loc));

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "refer");

  gcj::context *ctx = unit->get (unit->include_id (from_stack.include));
  gcj::jump_from jump_from;
  build_ref_jump_from (build_file_location (from_loc),
		       strlen (IDENTIFIER_POINTER (DECL_NAME (decl))),
		       unit, ctx, from_stack.macro,
		       &jump_from);

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "declare");

  gcj::jump_to jump_to;
  build_ref_jump_to (unit->include_id (to_stack.include),
		     build_file_location (to_loc),
		     unit, to_stack.macro,
		     &jump_to);

  ctx->add (jump_from, jump_to);
}

static void
cb_expand_macro (void *arg, void *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current ();

  if (! unit)
    return;

  const cpp_token *token = ((expand_macro_arg *) arg)->token;
  source_location from_loc = ((expand_macro_arg *) arg)->loc;
  source_location to_loc = ((expand_macro_arg *) arg)->macro_loc;

  assert (token->type == CPP_NAME);
  const char *name = (const char *) NODE_NAME (token->val.node.spelling);

  set->trace ("enter_macro %s %s:%d,%d\n",
	      name,
	      LOCATION_FILE (from_loc), LOCATION_LINE (from_loc),
	      LOCATION_COLUMN (from_loc));

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "macro");

  int iid = unit->include_id (from_stack.include);
  gcj::context *ctx;
  gcj::expansion_point point (iid,
			      build_source_location (unit, from_loc));
  int eid = unit->point_id (point);
  if (from_stack.macro.length () == 0)
    ctx = unit->get (iid);
  else
    ctx = unit->get (from_stack.macro.front ()->include, eid);

  set->trace ("enter_macro_context, macro %s defined at %s:%d,%d\n",
	      name,
	      LOCATION_FILE (to_loc), LOCATION_LINE (to_loc),
	      LOCATION_COLUMN (to_loc));

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "define");
  assert (to_stack.macro.length () == 0);

  gcj::jump_from jump_from (from_stack.macro.length () == 0
			    ? build_file_location (from_loc)
			    : from_stack.macro.front ()->loc.loc,
			    strlen (name));
  gcj::jump_to jump_to (unit->include_id (to_stack.include), eid,
			build_file_location (to_loc),
			0,
			from_stack.macro.length () == 0
			? unit->get_expansion () : 0);
  ctx->add (jump_from, jump_to);
}

static void
cb_finish (void *, void *data)
{
  gcj::set *set = (gcj::set *) data;
  set->trace ("finish\n");
  delete set;
}

int
plugin_init (plugin_name_args *plugin_info,
	     plugin_gcc_version *version)
{
  if (! plugin_default_version_check (version, &gcc_version))
    {
      fprintf (stderr, "This GCC plugin is for version %d.%d\n",
	       GCCPLUGIN_VERSION_MAJOR,
	       GCCPLUGIN_VERSION_MINOR);
      return 1;
    }

  const char *db = NULL;
  int flags = gcj::SF_LOAD | gcj::SF_SAVE;
  for (int i = 0; i < plugin_info->argc; ++ i)
    if (strcmp (plugin_info->argv[i].key, "db") == 0)
      db = plugin_info->argv[i].value;
    else if (strcmp (plugin_info->argv[i].key, "overwrite") == 0)
      flags &= ~gcj::SF_LOAD;
    else if (strcmp (plugin_info->argv[i].key, "trace") == 0)
      flags |= gcj::SF_TRACE;
    else if (strcmp (plugin_info->argv[i].key, "dump") == 0)
      flags |= gcj::SF_DUMP;

  if (! db)
    {
      fprintf (stderr, "Database not specified\n");
      return 1;
    }

  gcj::set *set = new gcj::set (db, flags);

  set->trace ("hello plugin\n");
  register_callback (plugin_info->base_name,
		     PLUGIN_START_UNIT,
		     cb_start_unit, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_CPP_TOKEN,
		     cb_cpp_token, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_EXTERNAL_REF,
		     cb_external_ref, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_EXPAND_MACRO,
		     cb_expand_macro, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH,
		     cb_finish, set);

  return 0;
}
