#include <stdio.h>
#include <assert.h>

#include <string>
#include <map>
#include <list>
#include <vector>

#include "gcc-plugin.h"
#include "plugin-version.h"
#include "output.h"
#include "toplev.h"
#include "opts.h"
#include "tree.h"
#include "print-tree.h"
#include "c-family/c-common.h"

int plugin_is_GPL_compatible;

static std::string
escape(const char *str, char c)
{
  std::string ret;
  for (const char *p = str; *p; ++p)
    {
      if (*p == '\\' || *p == c)
	ret += '\\';
      ret += *p;
    }
  return ret;
}

namespace gcj
{

template <typename type>
class id_map
{
public:
  id_map ()
    : cur (0)
  {
  }
  id_map (const id_map& map)
    : cur (map.cur), map (map.map)
  {
  }

  int
  get (const type& key)
  {
    if (map.find (key) == map.end ())
      map.insert (std::make_pair (key, ++cur));
    return map.find (key)->second;
  }

private:
  int cur;
  std::map<type, int> map;
};

struct unit;

struct file_location
{
  file_location ()
    : line (0), col (0)
  {
  }

  file_location (const file_location& loc)
    : line (loc.line), col (loc.col)
  {
  }

  file_location (int l, int c)
    : line (l), col (c)
  {
  }

  file_location (::source_location l);

  bool
  operator< (const file_location& rhs) const
  {
    return line < rhs.line
	   || (! (rhs.line < line) && col < rhs.col);
  }

  bool
  operator!= (const file_location &rhs) const
  {
    return line != rhs.line || col != rhs.col;
  }

  int line;
  int col;
};

struct source_location
{
  source_location (const source_location& loc)
    : fid (loc.fid), loc (loc.loc)
  {
  }

  source_location (unit *unit, ::source_location l);
  source_location (unit *unit, const line_map_ordinary *m);

  bool
  operator< (const source_location& rhs) const
  {
    return fid < rhs.fid
	   || (! (rhs.fid < fid) && loc < rhs.loc);
  }

  int fid;
  file_location loc;
};

struct macro_stack;

struct source_stack
{
  source_stack ()
  {
  }

  source_stack (const macro_stack& stack);

  bool
  operator< (const source_stack& rhs) const
  {
    return locs < rhs.locs;
  }

  void
  add (const source_location& loc)
  {
    locs.push_back (loc);
  }

  std::vector<source_location> locs;
};

struct expansion_point
{
  expansion_point (const expansion_point& point)
    : include (point.include), loc (point.loc)
  {
  }

  expansion_point (const source_stack& include,
		   const source_location& loc)
    : include (include), loc (loc)
  {
  }

  bool
  operator< (const expansion_point& rhs) const
  {
    return include < rhs.include
	   || (! (rhs.include < include) && loc < rhs.loc);
  }

  source_stack include;
  source_location loc;
};

struct macro_stack
{
  void
  add (const expansion_point& point)
  {
    points.push_back (point);
  }

  int
  length () const
  {
    return points.size ();
  }

  expansion_point *
  front ()
  {
    return &points.front ();
  }

  std::vector<expansion_point> points;
};

struct jump_from
{
  jump_from ()
    : len (0), expanded_id (0)
  {
  }

  jump_from (const jump_from& from)
    : loc (from.loc), len (from.len),
      expanded_id (from.expanded_id)
  {
  }

  jump_from (const file_location& loc, int len)
    : loc (loc), len (len), expanded_id (0)
  {
  }

  jump_from (const file_location& loc, int, int expanded_id)
    : loc (loc), len (0), expanded_id (expanded_id)
  {
  }

  bool
  operator< (const jump_from &rhs) const
  {
    return loc < rhs.loc
	   || (! (rhs.loc < loc)
	       && expanded_id < rhs.expanded_id);
  }

  file_location loc;
  int len;
  int expanded_id;
};

struct expanded_token
{
  expanded_token (const expanded_token& token)
    : token (token.token), id (token.id)
  {
  }

  expanded_token (const std::string& token, int id)
    : token (token), id (id)
  {
  }

  std::string token;
  int id;
};

struct expansion
{
  expansion ()
  {
  }

  expansion (const expansion& exp)
    : map (exp.map), tokens (exp.tokens)
  {
  }

  void
  add (const std::string& token, const source_stack& stack)
  {
    tokens.push_back (expanded_token (token, map.get (stack)));
  }

  int
  id (const source_stack& stack)
  {
    return map.get (stack);
  }

  id_map<source_stack> map;
  std::vector<expanded_token> tokens;
};

struct context;

struct jump_to
{
  jump_to ()
    : ctx (NULL), expanded_id (0), exp (NULL)
  {
  }

  jump_to (const jump_to& to)
    : ctx (to.ctx), loc (to.loc),
      expanded_id (to.expanded_id),
      exp (to.exp)
  {
  }

  jump_to (context *ctx,
	   const file_location& loc)
    : ctx (ctx), loc (loc),
      expanded_id (0), exp (NULL)
  {
  }

  jump_to (context *ctx,
	   const file_location& loc, expansion *exp)
    : ctx (ctx), loc (loc),
      expanded_id (0), exp (exp)
  {
  }

  jump_to (context *ctx,
	   const file_location& loc, int expanded_id)
    : ctx (ctx), loc (loc),
      expanded_id (expanded_id), exp (NULL)
  {
  }

  expansion *
  get_expansion ()
  {
    return exp;
  }

  context *ctx;
  file_location loc;
  int expanded_id;
  expansion *exp;
};

struct context
{
  context ()
    : id (0), surrounding (NULL)
  {
  }

  context (const context& ctx)
    : id (ctx.id), jumps (ctx.jumps),
      surrounding (ctx.surrounding),
      expansion_contexts (ctx.expansion_contexts)
  {
  }

  context *
  expansion (const expansion_point& point, int *id)
  {
    std::pair<std::map<expansion_point, context>::iterator, bool> pair;
    pair = expansion_contexts.insert (std::make_pair (point, context ()));
    if (pair.second)
      {
        pair.first->second.id = ++*id;
        pair.first->second.surrounding = this;
      }
    return &pair.first->second;
  }

  void
  add (const jump_from& from, const jump_to& to)
  {
    assert (jumps.insert (std::make_pair (from, to)).second);
  }

  jump_to *
  jump (const file_location& loc, int expanded_id)
  {
    jump_from from (loc, expanded_id);
    std::map<jump_from, jump_to>::iterator it = jumps.upper_bound (from);
    if (it == jumps.begin ())
      goto search_surrounding;
    --it;

    if (expanded_id != 0
        && (it->first.loc != loc || it->first.expanded_id != expanded_id))
      goto search_surrounding;

    if (it->first.loc.line != loc.line
	|| it->first.loc.col + it->first.len <= loc.col)
      goto search_surrounding;

    return &it->second;

  search_surrounding:
    if (surrounding)
      return surrounding->jump (loc, expanded_id);
    else
      return NULL;
  }

  int id;
  std::map<jump_from, jump_to> jumps;
  context *surrounding;

  std::map<expansion_point, context> expansion_contexts;
};

struct unit
{
  unit (const unit& unit)
    : args (unit.args),
      context_id (unit.context_id),
      contexts (unit.contexts),
      file_map (unit.file_map),
      stack_map (unit.stack_map)
  {
  }

  unit (const std::string& args)
    : args (args), context_id (0)
  {
  }

  context *
  get (const source_stack& include)
  {
    std::pair<std::map<source_stack, context>::iterator, bool> pair;
    pair = contexts.insert (std::make_pair (include, context ()));
    if (pair.second)
      pair.first->second.id = ++context_id;
    return &pair.first->second;
  }

  context *
  get (const source_stack& include,
       const expansion_point& point)
  {
    return get (include)->expansion (point, &context_id);
  }

  expansion *
  get_expansion ()
  {
    expansions.push_back (expansion ());
    return &expansions.back ();
  }

  int
  fileid (const char *file)
  {
    return file_map.get (file);
  }

  int
  stackid (const source_stack& stack)
  {
    return stack_map.get (stack);
  }

  std::string args;
  int context_id;
  std::map<source_stack, context> contexts;
  std::list<expansion> expansions;
  id_map<std::string> file_map;
  id_map<source_stack> stack_map;
};

struct set
{
  void
  next (const std::string& args)
  {
    units.push_back (unit (args));
  }

  unit *
  current ()
  {
    return &units.back ();
  }

  std::list<unit> units;
};

struct unwind_stack
{
  macro_stack macro;
  source_stack include;
};

file_location::file_location (::source_location l)
  : line (LOCATION_LINE (l)), col (LOCATION_COLUMN (l))
{
}

source_location::source_location (unit *unit, ::source_location l)
  : fid (unit->fileid (LOCATION_FILE (l))), loc (l)
{
}

source_location::source_location (unit *unit, const line_map_ordinary *m)
  : fid (unit->fileid (LINEMAP_FILE (m))),
    loc (LAST_SOURCE_LINE (m), 0)
{
}

source_stack::source_stack (const macro_stack& stack)
{
  std::vector<expansion_point>::const_iterator it;
  for (it = stack.points.begin (); it != stack.points.end (); ++it)
    locs.push_back (it->loc);
}

}

static void
cb_start_unit (void *, void *data)
{
  gcj::set *set = (gcj::set *) data;

  std::string args;
  for (size_t i = 1; i < save_decoded_options_count; ++i)
    {
      fprintf (stderr, "option: %lu %s %s",
	       save_decoded_options[i].opt_index,
	       save_decoded_options[i].arg,
	       save_decoded_options[i].orig_option_with_args_text);
      for (size_t j = 0; j < save_decoded_options[i].canonical_option_num_elements; ++j)
	{
	  fprintf (stderr, "\n  %s",
		   save_decoded_options[i].canonical_option[j]);

	  if (!args.empty()) args += ' ';
	  args += escape(save_decoded_options[i].canonical_option[j], ' ');
	}
      fprintf (stderr, "\n");
    }

  set->next (args);

  if (!flag_syntax_only)
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
  assert (loc > BUILTINS_LOCATION);

  const line_map_ordinary *m;
  linemap_resolve_location (line_table, loc,
			    LRK_MACRO_EXPANSION_POINT, &m);

  while (! MAIN_FILE_P (m))
    {
      m = INCLUDED_FROM (line_table, m);
      fprintf (stderr, "  %s, included from %s:%d\n",
	       prefix,
	       LINEMAP_FILE (m), LAST_SOURCE_LINE (m));

      stack->add (gcj::source_location (unit, m));
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
      fprintf (stderr, "  %s, expanded from %s:%d,%d\n",
	       prefix,
	       LOCATION_FILE (l), LOCATION_LINE (l), LOCATION_COLUMN (l));

      gcj::source_stack include;
      unwind_include (unit, l, &include, prefix);

      stack->add (gcj::expansion_point (include,
					gcj::source_location (unit, l)));
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
spell (const cpp_token_arg *token)
{
  return std::string (); // TODO
}

static void
cb_cpp_token (void *arg, void  *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current ();

  source_location loc = ((cpp_token_arg *) arg)->loc;

  gcj::unwind_stack stack;
  unwind_macro (unit, loc, &stack.macro, "cpp_token");

  if (stack.macro.length () == 0)
    return;

  fprintf (stderr, "cpp_token at %u %s:%d,%d\n",
	   loc,
	   LOCATION_FILE (loc), LOCATION_LINE (loc), LOCATION_COLUMN (loc));

  unwind_include (unit, loc, &stack.include, "cpp_token");

  // TOFIX, stack.include is not accurate
  gcj::context *ctx = unit->get (stack.include);
  gcj::jump_to *to = ctx->jump (gcj::file_location (loc), 0);
  assert (to && to->get_expansion ());
  to->get_expansion ()->add (spell ((cpp_token_arg *) arg),
			     gcj::source_stack (stack.macro));
}

static void
build_ref_jump_from (const gcj::file_location& loc,
		     int len,
		     gcj::context *ctx, const gcj::macro_stack& stack,
		     gcj::jump_from *from)
{
  if (stack.length () == 0)
    *from = gcj::jump_from (loc, len);
  else
    {
      gcj::jump_to *jump_to = ctx->jump (loc, 0);
      assert (jump_to && jump_to->get_expansion ());
      int id = jump_to->get_expansion ()->id (gcj::source_stack (stack));
      *from = gcj::jump_from (loc, 0, id);
    }
}

static void
build_ref_jump_to (gcj::context *ctx,
		   const gcj::file_location& loc,
		   const gcj::macro_stack& stack,
		   gcj::jump_to *to)
{
  if (stack.length () == 0)
    *to = gcj::jump_to (ctx, loc);
  else
    {
      gcj::jump_to *jump_to = ctx->jump (loc, 0);
      assert (jump_to && jump_to->get_expansion ());
      int id = jump_to->get_expansion ()->id (gcj::source_stack (stack));
      *to = gcj::jump_to (ctx, loc, id);
    }
}

static void
cb_external_ref (void *arg, void *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current();

  tree decl = ((external_ref_arg *) arg)->decl;
  source_location from_loc = ((external_ref_arg *) arg)->loc;
  source_location to_loc = DECL_SOURCE_LOCATION (decl);
  fprintf (stderr, "build_external_ref %s declared at %s:%d,%d\n",
	   IDENTIFIER_POINTER (DECL_NAME (decl)),
	   LOCATION_FILE (to_loc),
	   LOCATION_LINE (to_loc),
	   LOCATION_COLUMN (to_loc));
  
  fprintf (stderr, "refered by %s:%d,%d\n",
	   LOCATION_FILE (from_loc),
	   LOCATION_LINE (from_loc),
	   LOCATION_COLUMN (from_loc));

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "refer");

  gcj::context *from = unit->get (from_stack.include);
  gcj::jump_from jump_from;
  build_ref_jump_from (gcj::file_location (from_loc),
		       strlen (IDENTIFIER_POINTER (DECL_NAME (decl))),
		       from, from_stack.macro,
		       &jump_from);

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "declare");

  gcj::context *to = unit->get (to_stack.include);
  gcj::jump_to jump_to;
  build_ref_jump_to (to,
		     gcj::file_location (to_loc),
		     to_stack.macro,
		     &jump_to);

  from->add (jump_from, jump_to);
}

static void
cb_expand_macro (void *arg, void *data)
{
  gcj::set *set = (gcj::set *) data;
  gcj::unit *unit = set->current ();

  const cpp_token *token = ((expand_macro_arg *) arg)->token;
  source_location from_loc = ((expand_macro_arg *) arg)->loc;
  source_location to_loc = ((expand_macro_arg *) arg)->macro_loc;

  assert (token->type == CPP_NAME);
  const char *name = (const char *) NODE_NAME (token->val.node.spelling);

  fprintf (stderr, "enter_macro %u %s %s:%d,%d\n",
           from_loc,
	   name,
	   LOCATION_FILE (from_loc), LOCATION_LINE (from_loc),
	   LOCATION_COLUMN (from_loc));

  gcj::unwind_stack from_stack;
  unwind (unit, from_loc, &from_stack, "macro");

  gcj::context *from;
  gcj::expansion_point point (from_stack.include,
			      gcj::source_location (unit, from_loc));
  if (from_stack.macro.length () == 0)
    from = unit->get (from_stack.include);
  else
    from = unit->get (from_stack.macro.front ()->include, point);

  fprintf (stderr,
	   "enter_macro_context, macro %s defined at %s:%d,%d\n",
	   name,
	   LOCATION_FILE (to_loc), LOCATION_LINE (to_loc),
	   LOCATION_COLUMN (to_loc));

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "define");
  assert (to_stack.macro.length () == 0);
  gcj::context *to = unit->get (to_stack.include, point);

  gcj::jump_from jump_from (from_stack.macro.length () == 0
			    ? from_loc : from_stack.macro.front ()->loc.loc,
			    strlen (name));
  gcj::jump_to jump_to (to, gcj::file_location (to_loc),
			from_stack.macro.length () == 0
			? unit->get_expansion () : NULL);
  from->add (jump_from, jump_to);
}

static void
cb_finish (void *, void *data)
{
  fprintf (stderr, "finish\n");
  delete (gcj::set *) data;
}

int
plugin_init (plugin_name_args *plugin_info,
	     plugin_gcc_version *version)
{
  if (!plugin_default_version_check (version, &gcc_version))
    {
      fprintf (stderr, "This GCC plugin is for version %d.%d\n",
	       GCCPLUGIN_VERSION_MAJOR,
	       GCCPLUGIN_VERSION_MINOR);
      return 1;
    }

  gcj::set *set = new gcj::set;

  fprintf (stderr, "hello plugin\n");
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
