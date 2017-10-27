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

struct plug_data
{
  gcj::id_map<source_location> exp_map;

  std::map<std::string, std::vector<gcj::jump_src> > srcs;
  std::map<std::string, gcj::jump_tgt> tgts;

  std::map<source_location, source_location> tags;
  std::map<source_location, std::string> tag_refs;
  std::map<source_location, std::string> tag_defs;
};

static gcj::file_location
build_file_location (source_location l)
{
  return gcj::file_location (LOCATION_LINE (l), LOCATION_COLUMN (l));
}

static gcj::source_location
build_source_location (gcj::unit* unit, source_location l)
{
  return gcj::source_location (unit->file_id (LOCATION_FILE (l)),
			       build_file_location (l));
}

static gcj::source_location
build_source_location (gcj::unit* unit, const line_map_ordinary* m)
{
  return gcj::source_location (unit->file_id (LINEMAP_FILE (m)),
			       gcj::file_location (LAST_SOURCE_LINE (m),
						   0));
}

static void
cb_start_unit (void*, void* data)
{
  gcj::set* set = (gcj::set*) data;

  int cp = 0;
  std::map<unsigned int, int> priorities;
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

      if (opt_index == OPT_o)
	continue;

      if (opt_index == OPT_SPECIAL_input_file)
	{
	  input = save_decoded_options[i].arg;
	  continue;
	}

      int p;
      p = priorities.find (opt_index) == priorities.end()
	  ? 255 : priorities.find (opt_index)->second;

      if (arg_map.find (p) == arg_map.end ())
	arg_map.insert (std::make_pair (p, std::string ()));

      std::string* a = &arg_map.find (p)->second;
      for (size_t j = 0;
	   j < save_decoded_options[i].canonical_option_num_elements;
	   ++ j)
	{
	  set->trace ("\n  %s",
		      save_decoded_options[i].canonical_option[j]);

	  if (! a->empty ()) *a += ' ';
	  *a += escape (save_decoded_options[i].canonical_option[j],
			' ');
	}
      set->trace ("\n");
    }

  char* full = realpath (input.c_str (), NULL);
  std::string args = ! full ? input : full;
  std::map<int, std::string>::iterator it;
  for (it = arg_map.begin (); it != arg_map.end (); ++ it)
    args += ' ' + it->second;

  set->next (args, input);
  set->cur_data = (void*) new plug_data;
}

static void
internal_link (gcj::unit* unit, plug_data* plug)
{
  std::map<std::string, std::vector<gcj::jump_src> >::iterator it;
  for (it = plug->srcs.begin (); it != plug->srcs.end (); ++ it)
    {
      if (plug->tgts.find (it->first) == plug->tgts.end ())
	continue;

      const gcj::jump_to& to = plug->tgts.find (it->first)->second.to;
      std::vector<gcj::jump_src>::iterator jt;
      for (jt = it->second.begin (); jt != it->second.end (); ++ jt)
	unit->get (jt->include)->add (jt->from, to);
    }
}

static void resolve_tags (gcj::set* set, plug_data* plug);

static void
cb_finish_unit (void*, void* data)
{
  gcj::set* set = (gcj::set*) data;
  plug_data* plug = (plug_data*) set->cur_data;
  internal_link (set->current (), plug);
  resolve_tags (set, plug);
  delete plug;
  set->cur_data = NULL;

  if (! flag_syntax_only)
    {
      int unit_id = set->current_id ();

      section* sec;
      sec = get_section (".GCJ.plugin",
			 SECTION_DEBUG
			 | SECTION_MERGE
			 | (SECTION_ENTSIZE & 4),
			 NULL);
      switch_to_section (sec);

      const char* op = integer_asm_op (4, 0);
      fputs (op, asm_out_file);
      fprintf (asm_out_file,
	       HOST_WIDE_INT_PRINT_DEC, (long) unit_id);
      fputc ('\n', asm_out_file);
    }
}

static void
unwind_include (gcj::unit* unit, source_location loc,
		gcj::source_stack* stack, const char* prefix)
{
  assert (loc != UNKNOWN_LOCATION);

  int fid = unit->file_id (LOCATION_FILE (loc));
  stack->add (gcj::source_location (fid));

  const line_map_ordinary* m;
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
unwind_macro (gcj::unit* unit, source_location loc,
	      gcj::macro_stack* stack, const char* prefix)
{
  if (loc <= BUILTINS_LOCATION)
    return;

  source_location w = loc;
  const line_map* m;
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
unwind (gcj::unit* unit, source_location loc,
	gcj::unwind_stack* stack, const char* prefix)
{
  unwind_macro (unit, loc, &stack->macro, prefix);
  unwind_include (unit, loc, &stack->include, prefix);
}

static std::string
spell_integer (const_tree value)
{
  char buf[WIDE_INT_PRINT_BUFFER_SIZE];
  print_dec (value, buf, TYPE_SIGN (TREE_TYPE (value)));
  return buf;
}

static std::string
spell_real (const_tree value)
{
  REAL_VALUE_TYPE d;
  d = TREE_REAL_CST (value);
  char buf[60];
  real_to_decimal (buf, &d, sizeof (buf), 0, 1);
  return buf;
}

#define OP(e, s) s,
#define TK(e, s) #e,
static const char* spellings[N_TTYPES] = { TTYPE_TABLE };
#undef OP
#undef TK

static std::string
spell (cpp_ttype type, const_tree value)
{
  if (type != CPP_PRAGMA && value != NULL_TREE)
    {
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
	  const char* p;
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
  return spellings[type];
}

static void
lex_token (gcj::set* set,
	   cpp_ttype type, const_tree value, source_location loc)
{
  gcj::unit* unit = set->current ();
  plug_data* plug = (plug_data*) set->cur_data;

  gcj::unwind_stack stack;
  unwind_macro (unit, loc, &stack.macro, "cpp_token");

  if (stack.macro.length () == 0)
    return;

  std::string token = spell (type, value);
  set->trace ("cpp_token %s, at %s:%d,%d %s:%d,%d\n",
	      token.c_str (),
	      LOCATION_FILE (loc), LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc),
	      expand_location_to_spelling_point (loc).file,
	      expand_location_to_spelling_point (loc).line,
	      expand_location_to_spelling_point (loc).column);

  unwind_include (unit, loc, &stack.include, "cpp_token");

  gcj::context* ctx = unit->get (unit->include_id (stack.include));
  gcj::jump_to* to = ctx->jump (unit, build_file_location (loc), 0);
  if (to && to->exp)
    unit->get_expansion (to->exp)->add (token,
					plug->exp_map.get (loc));
  // TOFIX locations of some tokens expanded from builtin macro
  // are not accurate
  else
    set->warning ("token expanded with inaccurate location"
		  "cpp_token %s, at %s:%d,%d",
	 	  token.c_str (),
		  LOCATION_FILE (loc), LOCATION_LINE (loc),
		  LOCATION_COLUMN (loc));
}

static void
build_ref_jump_from (source_location loc, int len,
		     const gcj::set* set,
		     gcj::context* ctx, const gcj::macro_stack& stack,
		     gcj::jump_from* from)
{
  gcj::file_location file_loc (build_file_location (loc));
  const gcj::unit* unit = set->current ();
  const plug_data* plug = (plug_data*) set->cur_data;
  if (stack.length () == 0)
    *from = gcj::jump_from (file_loc, len);
  else
    {
      gcj::jump_to* jump_to = ctx->jump (unit, file_loc, 0);
      assert (jump_to && jump_to->exp);
      int expid = plug->exp_map.get (loc);
      int id = unit->get_expansion (jump_to->exp)->id (expid);
      *from = gcj::jump_from (file_loc, 0, id);
    }
}

static void
build_ref_jump_to (int unit_id, const gcj::unwind_stack& stack,
		   source_location loc, gcj::set* set,
		   gcj::jump_to* to)
{
  gcj::file_location file_loc (build_file_location (loc));
  gcj::unit* unit = set->current ();
  const plug_data* plug = (plug_data*) set->cur_data;
  int include = unit->include_id (stack.include);
  if (stack.macro.length () == 0)
    *to = gcj::jump_to (unit_id, include, 0, file_loc);
  else
    {
      const gcj::context* ctx = unit->get (include);
      const gcj::jump_to* jump_to = ctx->jump (unit, file_loc, 0, NULL);
      assert (jump_to && jump_to->exp);
      int expid = plug->exp_map.get (loc);
      int id = unit->get_expansion (jump_to->exp)->id (expid);
      *to = gcj::jump_to (unit_id, include, 0, file_loc, id);
    }
}

static void
build_ref (gcj::set* set, const_tree ref, source_location loc)
{
  gcj::unit* unit = set->current();

  source_location from_loc = loc;
  source_location to_loc = DECL_SOURCE_LOCATION (ref);
  if (to_loc == UNKNOWN_LOCATION)
    return;
  const char* name = IDENTIFIER_POINTER (DECL_NAME (ref));
  set->trace ("build_ref %s declared at %s:%d,%d\n",
	      name,
	      LOCATION_FILE (to_loc),
	      LOCATION_LINE (to_loc),
	      LOCATION_COLUMN (to_loc));
  
  set->trace ("refered by %s:%d,%d\n",
	      LOCATION_FILE (from_loc),
	      LOCATION_LINE (from_loc),
	      LOCATION_COLUMN (from_loc));

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "refer");

  gcj::context* ctx = unit->get (unit->include_id (from_stack.include));
  gcj::jump_from jump_from;
  build_ref_jump_from (from_loc, strlen (name), set,
		       ctx, from_stack.macro, &jump_from);

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "declare");

  gcj::jump_to jump_to;
  build_ref_jump_to (set->current_id (), to_stack,
		     to_loc, set, &jump_to);

  ctx->add (jump_from, jump_to);
}

static void
expand_macro (gcj::set* set, const cpp_token* token,
	      source_location loc, source_location macro_loc)
{
  gcj::unit* unit = set->current ();

  source_location from_loc = loc;
  source_location to_loc = macro_loc;

  assert (token->type == CPP_NAME);
  const char* name = (const char*) NODE_NAME (token->val.node.spelling);

  set->trace ("enter_macro %s %s:%d,%d %s:%d,%d\n",
	      name,
	      LOCATION_FILE (from_loc), LOCATION_LINE (from_loc),
	      LOCATION_COLUMN (from_loc),
	      expand_location_to_spelling_point (from_loc).file,
	      expand_location_to_spelling_point (from_loc).line,
	      expand_location_to_spelling_point (from_loc).column);

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "macro");

  source_location spell_loc;
  spell_loc = linemap_resolve_location (line_table, from_loc,
                                       LRK_SPELLING_LOCATION, NULL);
  gcj::unwind_stack spell_stack;
  unwind (unit, spell_loc, &spell_stack, "macro_spell");

  // TOFIX better handle pasting
  if (token->flags & PASTED)
    {
      set->trace ("macro %s results from pasting\n", name);
      return;
    }

  gcj::context* ctx;
  gcj::expansion_point point (unit->include_id (from_stack.include),
			      build_source_location (unit, from_loc));
  int eid = unit->point_id (point);

  if (from_stack.macro.length () == 0)
    assert (from_loc == spell_loc);

  // Use spell_loc instead of from_stack.macro.front() to handle
  //   #define A(a) a
  //   #define B(a) a
  //   #define M(t, a) t(a)
  //   #define E (M(A, 0), M(B, 0))
  //   E
  // in which case, from_stack.macro.front() both are t(a)
  if (from_stack.macro.length () == 0)
    ctx = unit->get (unit->include_id (from_stack.include));
  else
    //ctx = unit->get (from_stack.macro.front()->include, eid);
    ctx = unit->get (unit->include_id (spell_stack.include), eid);

  set->trace ("enter_macro_context, macro %s defined at %s:%d,%d\n",
	      name,
	      LOCATION_FILE (to_loc), LOCATION_LINE (to_loc),
	      LOCATION_COLUMN (to_loc));

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "define");
  assert (to_stack.macro.length () == 0);

  gcj::jump_from jump_from (build_file_location (spell_loc),
			    strlen (name));
  int to_include = unit->include_id (to_stack.include);
  // Touch the context so it gets surrounding
  unit->get (to_include, eid);

  int exp = 0;
  // Outmost macros probably shall not be expanded more than
  // once, but it does happen when compiling a linux-kernel
  if (from_stack.macro.length () == 0)
    exp = ctx->jumps.find (jump_from) != ctx->jumps.end ()
	    ? ctx->jumps.find (jump_from)->second.exp
	    : unit->get_expansion ();
  gcj::jump_to jump_to (set->current_id (), to_include, eid,
			build_file_location (to_loc),
			0, exp);
  ctx->add (jump_from, jump_to);
}

static void
stack_file (gcj::set* set, source_location loc, const char* file)
{
  gcj::unit* unit = set->current ();

  set->trace ("stack file from %s:%d,%d to %s\n",
	      LOCATION_FILE (loc),
	      LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc), file);

  gcj::unwind_stack stack;
  unwind (unit, loc, &stack, "stack");

  gcj::context* ctx = unit->get (unit->include_id (stack.include));
  assert (LOCATION_COLUMN (loc) == 0);
  gcj::jump_from jump_from (build_file_location (loc), 0);

  gcj::source_stack to;
  to.add (gcj::source_location (unit->file_id (file)));
  to.add (build_source_location (unit, loc));
  to.locs.insert (to.locs.end (),
		  stack.include.locs.begin () + 1,
		  stack.include.locs.end ());
  gcj::jump_to jump_to (set->current_id (),
			unit->include_id (to), 0,
			gcj::file_location ());

  ctx->add (jump_from, jump_to);
}

static void
ref_tag (gcj::set* set, const_tree name,
	 source_location loc, source_location ref_loc)
{
  set->trace ("ref_tag %s refered to %s:%d,%d, by %s:%d,%d",
	      IDENTIFIER_POINTER (name),
	      LOCATION_FILE (ref_loc), LOCATION_LINE (ref_loc),
	      LOCATION_COLUMN (ref_loc),
	      LOCATION_FILE (loc), LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc));

  plug_data* plug = (plug_data*) set->cur_data;
  assert (plug->tags.insert (std::make_pair (loc, ref_loc)).second);
  assert (plug->tag_refs.insert (
	std::make_pair (loc, IDENTIFIER_POINTER (name))).second);
}

static void
ref_start_tag (gcj::set* set, const_tree name,
	       source_location loc, source_location ref_loc)
{
  set->trace ("ref_start_tag %s refered to %s:%d,%d, by %s:%d,%d",
	      IDENTIFIER_POINTER (name),
	      LOCATION_FILE (ref_loc), LOCATION_LINE (ref_loc),
	      LOCATION_COLUMN (ref_loc),
	      LOCATION_FILE (loc), LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc));

  plug_data* plug = (plug_data*) set->cur_data;
  assert (plug->tags.insert (std::make_pair (loc, ref_loc)).second);
  assert (plug->tag_defs.insert (
	std::make_pair (loc, IDENTIFIER_POINTER (name))).second);
}

static void
resolve_tags (gcj::set* set, plug_data* plug)
{
  gcj::unit* unit = set->current ();

  std::map<source_location, std::string>::iterator it;
  for (it = plug->tag_defs.begin (); it != plug->tag_defs.end (); ++ it)
    if (plug->tags.find (it->first) != plug->tags.end ())
      {
	source_location loc = plug->tags.find (it->first)->second;
	if (plug->tags.find (loc) != plug->tags.end ())
	  plug->tags.erase (loc);
	plug->tags.insert (std::make_pair (loc, it->first));
	plug->tags.erase (it->first);
      }

  std::map<source_location, gcj::jump_to> tos;
  std::map<source_location, source_location>::iterator jt;
  for (jt = plug->tags.begin (); jt != plug->tags.end (); ++ jt)
    {
      std::map<source_location, source_location>::iterator kt = jt;
      while (plug->tags.find (kt->second) != plug->tags.end ())
	kt = plug->tags.find (kt->second);

      const char* name;
      if (plug->tag_defs.find (kt->second) != plug->tag_defs.end ())
	name = plug->tag_defs.find (kt->second)->second.c_str ();
      else
	name = plug->tag_refs.find (kt->first)->second.c_str ();

      // build jump_from from jt->first
      gcj::unwind_stack from_stack;
      unwind (unit, jt->first, &from_stack, "ref_tag_from");
      gcj::context* ctx = unit->get (unit->include_id (from_stack.include));
      gcj::jump_from jump_from;
      build_ref_jump_from (jt->first, strlen (name), set,
			   ctx, from_stack.macro, &jump_from);

      if (tos.find (kt->second) != tos.end ())
        {
          ctx->add (jump_from, tos.find (kt->second)->second);
	  continue;
        }

      gcj::unwind_stack to_stack;
      unwind (unit, kt->second, &to_stack, "ref_tag_to");

      gcj::jump_to jump_to;
      build_ref_jump_to (set->current_id (), to_stack,
			 kt->second, set, &jump_to);
      ctx->add (jump_from, jump_to);
      tos.insert (std::make_pair (kt->second, jump_to));
    }
}

static void
cb_build_gcc_jump (void* arg, void* data)
{
  build_gcc_jump_arg* gcj_arg = (build_gcc_jump_arg*) arg;
  gcj::set* set = (gcj::set*) data;
  if (gcj_arg->type == GCC_JUMP_LEX_TOKEN)
    lex_token (set,
	       gcj_arg->u.lex_token.type,
	       gcj_arg->u.lex_token.value,
	       gcj_arg->u.lex_token.loc);
  else if (gcj_arg->type == GCC_JUMP_BUILD_REF)
    build_ref (set,
	       gcj_arg->u.build_ref.ref,
	       gcj_arg->u.build_ref.loc);
  else if (gcj_arg->type == GCC_JUMP_EXPAND_MACRO)
    expand_macro (set,
		  gcj_arg->u.expand_macro.token,
		  gcj_arg->u.expand_macro.loc,
		  gcj_arg->u.expand_macro.macro_loc);
  else if (gcj_arg->type == GCC_JUMP_STACK_FILE)
    stack_file (set,
		gcj_arg->u.stack_file.loc,
		gcj_arg->u.stack_file.file);
  else if (gcj_arg->type == GCC_JUMP_REF_TAG)
    ref_tag (set,
	     gcj_arg->u.ref_tag.name,
	     gcj_arg->u.ref_tag.loc,
	     gcj_arg->u.ref_tag.ref_loc);
  else if (gcj_arg->type == GCC_JUMP_REF_START_TAG)
    ref_start_tag (set,
		   gcj_arg->u.ref_tag.name,
		   gcj_arg->u.ref_tag.loc,
		   gcj_arg->u.ref_tag.ref_loc);
  else
    assert (false);
}

static void
add_jump_src (std::map<std::string, std::vector<gcj::jump_src> >* srcs,
	      const std::string& name, int include,
	      const gcj::jump_from& jump_from)
{
  if (srcs->find (name) == srcs->end ())
    srcs->insert (std::make_pair (name, std::vector<gcj::jump_src> ()));
  srcs->find (name)->second.push_back (gcj::jump_src (include, jump_from));
}

static void
add_jump_tgt (std::map<std::string, gcj::jump_tgt>* tgts,
	      const std::string& name,
	      const gcj::jump_to& jump_to, bool weak)
{
  if (tgts->find (name) == tgts->end ())
    tgts->insert (std::make_pair (name, gcj::jump_tgt (jump_to, weak)));
  // We could done more check here
  else if (tgts->find (name)->second.weak && ! weak)
    tgts->find (name)->second = gcj::jump_tgt (jump_to, weak);
}

static void
add_declaration (gcj::set* set, tree decl)
{
  gcj::unit* unit = set->current ();
  plug_data* plug = (plug_data*) set->cur_data;

  source_location loc = DECL_SOURCE_LOCATION (decl);
  if (loc == UNKNOWN_LOCATION)
    return;
  const char* name = IDENTIFIER_POINTER (DECL_NAME (decl));

  gcj::unwind_stack stack;
  unwind (unit, loc, &stack, "add_decl");

  int include = unit->include_id (stack.include);
  gcj::jump_from jump_from;
  build_ref_jump_from (loc, strlen (name), set,
		       unit->get (include), stack.macro, &jump_from);

  add_jump_src (TREE_PUBLIC (decl) ? &unit->pub_srcs : &plug->srcs,
		name, include, jump_from);
}

static void
add_definition (gcj::set* set, tree decl)
{
  int unit_id = set->current_id ();
  gcj::unit* unit = set->current ();
  plug_data* plug = (plug_data*) set->cur_data;

  source_location loc = DECL_SOURCE_LOCATION (decl);
  if (loc == UNKNOWN_LOCATION)
    return;
  const char* name = IDENTIFIER_POINTER (DECL_NAME (decl));

  gcj::unwind_stack stack;
  unwind (unit, loc, &stack, "add_decl");

  gcj::jump_to jump_to;
  build_ref_jump_to (unit_id, stack, loc, set, &jump_to);

  add_jump_tgt (TREE_PUBLIC (decl) ? &unit->pub_tgts : &plug->tgts,
		name, jump_to, DECL_WEAK (decl));
}

static void
cb_finish_decl (void* arg, void* data)
{
  tree decl = (tree) arg;
  gcj::set* set = (gcj::set*) data;
  //gcj::unit* unit = set->current (); TODO

  if (DECL_NAME (decl) != NULL_TREE
      && VAR_OR_FUNCTION_DECL_P (decl)
      && DECL_FILE_SCOPE_P (decl))
    {
      set->trace ("finish_parse_decl %s %s:%d,%d "
		  "external %d public %d initial %d\n",
		  IDENTIFIER_POINTER (DECL_NAME (decl)),
		  DECL_SOURCE_FILE (decl), DECL_SOURCE_LINE (decl),
		  DECL_SOURCE_COLUMN (decl), DECL_EXTERNAL (decl),
		  TREE_PUBLIC (decl), DECL_INITIAL (decl) != NULL_TREE);

      if (DECL_INITIAL (decl) != NULL_TREE)
	add_definition (set, decl);
      else
	add_declaration (set, decl);
    }
}

static void
cb_finish_parse_function (void* arg, void* data)
{
  tree func = (tree) arg;
  gcj::set* set = (gcj::set*) data;
  // XXX Calling here IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (func))
  // leads to crash on lang_hooks.set_decl_assembler_name
  set->trace ("finish_parse_function %s %s:%d,%d public %d\n",
	      IDENTIFIER_POINTER (DECL_NAME (func)),
	      DECL_SOURCE_FILE (func), DECL_SOURCE_LINE (func),
	      DECL_SOURCE_COLUMN (func), TREE_PUBLIC (func));
  add_definition (set, func);
}

static void
cb_finish (void*, void* data)
{
  gcj::set* set = (gcj::set*) data;
  set->trace ("finish\n");
  delete set;
}

int
plugin_init (plugin_name_args* plugin_info,
	     plugin_gcc_version* version)
{
  if (! plugin_default_version_check (version, &gcc_version))
    {
      fprintf (stderr, "This GCC plugin is for version %d.%d\n",
	       GCCPLUGIN_VERSION_MAJOR,
	       GCCPLUGIN_VERSION_MINOR);
      return 1;
    }

  const char* db = NULL;
  int flags = 0;
  for (int i = 0; i < plugin_info->argc; ++ i)
    if (strcmp (plugin_info->argv[i].key, "db") == 0)
      db = plugin_info->argv[i].value;
    else if (strcmp (plugin_info->argv[i].key, "trace") == 0)
      flags |= gcj::SF_TRACE;
    else if (strcmp (plugin_info->argv[i].key, "dump") == 0)
      flags |= gcj::SF_DUMP;

  if (! db)
    {
      fprintf (stderr, "GCJ database not specified\n");
      return 1;
    }

  gcj::set* set = new gcj::set (db, flags);

  set->trace ("hello plugin\n");
  register_callback (plugin_info->base_name,
		     PLUGIN_START_UNIT,
		     cb_start_unit, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH_UNIT,
		     cb_finish_unit, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_BUILD_GCC_JUMP,
		     cb_build_gcc_jump, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH_DECL,
		     cb_finish_decl, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH_PARSE_FUNCTION,
		     cb_finish_parse_function, set);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH,
		     cb_finish, set);

  return 0;
}
