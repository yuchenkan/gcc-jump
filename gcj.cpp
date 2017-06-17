#include "gcj.hpp"

std::string
escape (const char *str, char c)
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

source_stack::source_stack (const macro_stack& stack)
{
  std::vector<expansion_point>::const_iterator it;
  for (it = stack.points.begin (); it != stack.points.end (); ++ it)
    locs.push_back (it->loc);
}

const jump_to *
context::jump (const file_location& loc, int expanded_id) const
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

void
context::dump (FILE *fp, int indet) const
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

void
unit::dump (FILE *fp, int indet) const
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
set::dump (FILE *fp, int indet) const
{
  std::map<int, unit>::const_iterator unit;
  for (unit = units.begin (); unit != units.end (); ++ unit)
    {
      iprintf (fp, indet, "unit %s:\n",
	       unit_map.at (unit->first).c_str ());
      unit->second.dump (fp, indet + 1);
    }
}


}
