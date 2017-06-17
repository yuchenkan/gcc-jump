#include <stdio.h>
#include <assert.h>
#include <stdarg.h>

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
#include "cpplib.h"
#include "wide-int-print.h"
#include "real.h"

int plugin_is_GPL_compatible;

static std::string
escape(const char *str, char c)
{
  std::string ret;
  for (const char *p = str; *p; ++ p)
    {
      if (*p == '\\' || *p == c)
	ret += '\\';
      ret += *p;
    }
  return ret;
}

struct logger
{
  logger (FILE *fp, bool enabled)
  : fp (fp), enabled (enabled)
  {
  }

  void
  vtrace (const char *fmt, va_list ap)
  {
    if (enabled)
      vfprintf (fp, fmt, ap);
  }

  FILE *fp;
  bool enabled;
};

namespace gcj
{

void
iprintf (FILE *fp, int i, const char *fmt, ...)
{
  for (int n = 0; n < i; ++ n)
    fprintf (fp, "  ");

  va_list ap;
  va_start (ap, fmt);
  vfprintf (fp, fmt, ap);
  va_end (ap);
}

template <typename type>
struct id_map
{
  id_map ()
    : cur (0)
  {
  }
  id_map (const id_map& map)
    : cur (map.cur), map (map.map), rmap (map.rmap)
  {
  }

  int
  get (const type& key) const
  {
    return map.find (key) == map.end ()
	   ? 0 : map.find (key)->second;
  }

  int
  get (const type& key)
  {
    if (map.find (key) == map.end ())
    {
      map.insert (std::make_pair (key, ++ cur));
      rmap.insert (std::make_pair (cur, &map.find (key)->first));
    }
    return map.find (key)->second;
  }

  const type&
  at (int id) const
  {
    assert (rmap.find (id) != rmap.end ());
    return *rmap.find (id)->second;
  }

  int cur;
  std::map<type, int> map;
  std::map<int, const type *> rmap;
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

  source_location (int fid)
    : fid (fid)
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

  expansion_point (int include,
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

  int include;
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
  add (const std::string& token, const source_stack& exp)
  {
    tokens.push_back (expanded_token (token, map.get (exp)));
  }

  int
  id (const source_stack& stack) const
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
    : include (0), point (0), expanded_id (0), exp (NULL)
  {
  }

  jump_to (const jump_to& to)
    : include (to.include), point (to.point),
      loc (to.loc), expanded_id (to.expanded_id),
      exp (to.exp)
  {
  }

  jump_to (int include, int point,
	   const file_location& loc)
    : include (include), point (point), loc (loc),
      expanded_id (0), exp (NULL)
  {
  }

  jump_to (int include, int point,
	   const file_location& loc, expansion *exp)
    : include (include), point (point), loc (loc),
      expanded_id (0), exp (exp)
  {
  }

  jump_to (int include, int point,
	   const file_location& loc, int expanded_id)
    : include (include), point (point), loc (loc),
      expanded_id (expanded_id), exp (NULL)
  {
  }

  const expansion *
  get_expansion () const
  {
    return exp;
  }

  expansion *
  get_expansion ()
  {
    return (expansion *) ((const jump_to *) this)->get_expansion ();
  }

  int include;
  int point;

  file_location loc;
  int expanded_id;

  expansion *exp;
};

struct context
{
  context ()
    : surrounding (NULL)
  {
  }

  context (const context& ctx)
    : jumps (ctx.jumps),
      surrounding (ctx.surrounding),
      expansion_contexts (ctx.expansion_contexts)
  {
  }

  const context *
  expansion (int point) const
  {
    return expansion_contexts.find (point) == expansion_contexts.end ()
	   ? NULL : &expansion_contexts.find (point)->second;
  }

  context *
  expansion (int point)
  {
    std::pair<std::map<int, context>::iterator, bool> pair;
    pair = expansion_contexts.insert (std::make_pair (point, context ()));
    if (pair.second)
      pair.first->second.surrounding = this;
    return &pair.first->second;
  }

  void
  add (const jump_from& from, const jump_to& to)
  {
    assert (jumps.insert (std::make_pair (from, to)).second);
  }

  const jump_to *
  jump (const file_location& loc, int expanded_id) const
  {
    jump_from from (loc, expanded_id);
    std::map<jump_from, jump_to>::const_iterator it;
    it = jumps.upper_bound (from);
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

  jump_to *
  jump (const file_location& loc, int expanded_id)
  {
    return (jump_to *) ((const context *) this)->jump (loc, expanded_id);
  }

  void
  dump (FILE *fp, int indet) const
  {
    iprintf (fp, indet, "jumps:\n");
    std::map<jump_from, jump_to>::const_iterator jump;
    for (jump = jumps.begin (); jump != jumps.end (); ++ jump)
      {
	iprintf (fp, indet + 1, "line,col: %d,%d %s:%d",
		 jump->first.loc.line,
		 jump->first.loc.col,
		 jump->first.len
		 ? "len" : "exp",
		 jump->first.len
		 ? jump->first.len : jump->first.expanded_id);

	if (jump->second.include)
	  {
	    fprintf (fp, " => ");
	    if (jump->second.point)
	      fprintf (fp, "expansion context: %d.%d",
		       jump->second.include, jump->second.point);
	    else
	      fprintf (fp, "context: %d", jump->second.include);
	    fprintf (fp, " line,col: %d,%d",
		     jump->second.loc.line,
		     jump->second.loc.col);
	    if (jump->second.expanded_id != 0)
	      fprintf (fp, " exp:%d", jump->second.expanded_id);
	  }

	fprintf (fp, "\n");

	if (jump->second.exp)
	  {
	    iprintf (fp, indet + 2, "expanded tokens:");

	    std::vector<expanded_token>::const_iterator tok;
	    for (tok = jump->second.exp->tokens.begin ();
		 tok != jump->second.exp->tokens.end ();
		 ++ tok)
	    fprintf (fp, " %d \"%s\"",
		     tok->id,
		     escape (tok->token.c_str (), '"').c_str ());
	    fprintf (fp, "\n");
	  }
      }

    std::map<int, context>::const_iterator ctx;
    for (ctx = expansion_contexts.begin ();
	 ctx != expansion_contexts.end ();
	 ++ ctx)
      {
	iprintf (fp, indet, "expansion context %d:\n",
		 ctx->first);
	ctx->second.dump (fp, indet + 1);
      }
  }


  int id;
  std::map<jump_from, jump_to> jumps;
  context *surrounding;

  std::map<int, context> expansion_contexts;
};

struct unit
{
  unit (const unit& unit)
    : log (unit.log),
      contexts (unit.contexts),
      file_map (unit.file_map),
      include_map (unit.include_map),
      expansion_map (unit.expansion_map)
  {
  }

  unit (logger *log)
    : log (log)
  {
  }

  const context *
  get (int include) const
  {
    return contexts.find (include) == contexts.end ()
	   ? NULL : &contexts.find (include)->second;
  }

  context *
  get (int include)
  {
    contexts.insert (std::make_pair (include, context ()));
    return &contexts.find (include)->second;
  }

  const context *
  get (int include, int point) const
  {
    return get (include)->expansion (point);
  }

  context *
  get (int include, int point)
  {
    return get (include)->expansion (point);
  }

  expansion *
  get_expansion ()
  {
    expansions.push_back (expansion ());
    return &expansions.back ();
  }

  int
  file_id (const char *file)
  {
    return file_map.get (file);
  }

  int
  include_id (const source_stack& include)
  {
    return include_map.get (include);
  }

  int
  expansion_id (const expansion_point& point)
  {
    return expansion_map.get (point);
  }

  void
  dump (FILE *fp, int indet) const
  {
    std::map<int, const source_stack*>::const_iterator inc;
    for (inc = include_map.rmap.begin ();
	 inc != include_map.rmap.end ();
	 ++ inc)
      {
	iprintf (fp, indet, "include %d:\n", inc->first);
	std::vector<source_location>::const_iterator loc;
	for (loc = inc->second->locs.begin ();
	     loc != inc->second->locs.end ();
	     ++ loc)
	  iprintf (fp, indet + 1, "from %s:%d,%d\n",
		   file_map.at (loc->fid).c_str (),
		   loc->loc.line, loc->loc.col);
      }

    std::map<int, context>::const_iterator ctx;
    for (ctx = contexts.begin (); ctx != contexts.end (); ++ ctx)
      {
	iprintf (fp, indet, "context %d:\n", ctx->first);
        ctx->second.dump (fp, indet + 1);
      }
  }

  void
  trace (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log->vtrace (fmt, ap);
    va_end (ap);
  }

  logger *log;
  std::map<int, context> contexts;
  std::list<expansion> expansions;
  id_map<std::string> file_map;
  id_map<source_stack> include_map;
  id_map<expansion_point> expansion_map;
};

struct set
{
  set ()
    : log (stderr, false)
  {
  }

  set (FILE *fp)
    : log (stderr, false)
  {
    load (fp);
  }

  void
  next (const std::string& args)
  {
    int id = unit_map.get (args);
    if (units.find (id) == units.end ())
      {
	units.insert (std::make_pair (id, unit (&log)));
	cur = &units.find (id)->second;
      }
    else
      cur = NULL;
  }

  unit *
  current () const
  {
    return cur;
  }

  const unit *
  get (int id) const
  {
    return NULL; // TODO
  }

  void
  dump (FILE *fp, int indet) const
  {
    std::map<int, unit>::const_iterator unit;
    for (unit = units.begin (); unit != units.end (); ++ unit)
      {
	iprintf (fp, indet, "unit %s:\n",
		 unit_map.at (unit->first).c_str ());
	unit->second.dump (fp, indet + 1);
      }
  }

  void
  save (FILE *fp) const
  {
  }

  void
  load (FILE *fp)
  {
    // TODO
  }

  void
  trace (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log.vtrace (fmt, ap);
    va_end (ap);
  }

  logger log;
  id_map<std::string> unit_map;
  std::map<int, unit> units;
  unit *cur;
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
  : fid (unit->file_id (LOCATION_FILE (l))), loc (l)
{
}

source_location::source_location (unit *unit, const line_map_ordinary *m)
  : fid (unit->file_id (LINEMAP_FILE (m))),
    loc (LAST_SOURCE_LINE (m), 0)
{
}

source_stack::source_stack (const macro_stack& stack)
{
  std::vector<expansion_point>::const_iterator it;
  for (it = stack.points.begin (); it != stack.points.end (); ++ it)
    locs.push_back (it->loc);
}

}

static void
cb_start_unit (void *, void *data)
{
  gcj::set *set = (gcj::set *) data;

  std::string args;
  for (size_t i = 1; i < save_decoded_options_count; ++ i)
    {
      set->trace ("option: %lu %s %s",
		  save_decoded_options[i].opt_index,
		  save_decoded_options[i].arg,
		  save_decoded_options[i].orig_option_with_args_text);
      for (size_t j = 0; j < save_decoded_options[i].canonical_option_num_elements; ++ j)
	{
	  set->trace ("\n  %s",
		      save_decoded_options[i].canonical_option[j]);

	  if (! args.empty()) args += ' ';
	  args += escape(save_decoded_options[i].canonical_option[j], ' ');
	}
      set->trace ("\n");
    }

  set->next (args);

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
  assert (loc > BUILTINS_LOCATION);

  int fid = unit->file_id (LOCATION_FILE (loc));
  stack->add (gcj::source_location (fid));

  const line_map_ordinary *m;
  linemap_resolve_location (line_table, loc,
			    LRK_MACRO_EXPANSION_POINT, &m);

  while (! MAIN_FILE_P (m))
    {
      m = INCLUDED_FROM (line_table, m);
      unit->trace ("  %s, included from %s:%d\n",
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
      unit->trace ("  %s, expanded from %s:%d,%d\n",
		   prefix,
		   LOCATION_FILE (l), LOCATION_LINE (l),
		   LOCATION_COLUMN (l));

      gcj::source_stack include;
      unwind_include (unit, l, &include, prefix);

      stack->add (gcj::expansion_point (unit->include_id (include),
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

  set->trace ("cpp_token at %s:%d,%d\n",
	      LOCATION_FILE (loc), LOCATION_LINE (loc),
	      LOCATION_COLUMN (loc));

  unwind_include (unit, loc, &stack.include, "cpp_token");

  gcj::context *ctx = unit->get (unit->include_id (stack.include));
  gcj::jump_to *to = ctx->jump (gcj::file_location (loc), 0);
  assert (to && to->get_expansion ());
  gcj::source_stack exp (stack.macro);
  std::string token = spell ((cpp_token_arg *) arg);
  to->get_expansion ()->add (token, exp);
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
      gcj::source_stack exp (stack);
      int id = jump_to->get_expansion ()->id (exp);
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
      const gcj::jump_to *jump_to = ctx->jump (loc, 0);
      assert (jump_to && jump_to->get_expansion ());
      int id = jump_to->get_expansion ()->id (gcj::source_stack (stack));
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
  build_ref_jump_from (gcj::file_location (from_loc),
		       strlen (IDENTIFIER_POINTER (DECL_NAME (decl))),
		       ctx, from_stack.macro,
		       &jump_from);

  gcj::unwind_stack to_stack;
  unwind (unit, to_loc, &to_stack, "declare");

  gcj::jump_to jump_to;
  build_ref_jump_to (unit->include_id (to_stack.include),
		     gcj::file_location (to_loc),
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
			      gcj::source_location (unit, from_loc));
  int eid = unit->expansion_id (point);
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
			    ? from_loc : from_stack.macro.front ()->loc.loc,
			    strlen (name));
  gcj::jump_to jump_to (unit->include_id (to_stack.include), eid,
			gcj::file_location (to_loc),
			from_stack.macro.length () == 0
			? unit->get_expansion () : NULL);
  ctx->add (jump_from, jump_to);
}

static void
cb_finish (void *, void *data)
{
  gcj::set *set = (gcj::set *) data;
  set->trace ("finish\n");
  set->dump (stderr, 0);
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

  gcj::set *set = new gcj::set;

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
