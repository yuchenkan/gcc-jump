#include <errno.h>
#include <limits.h>
#include <stdlib.h>

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

static void
save_uint64 (FILE *fp, uint64_t v)
{
  assert (fwrite (&v, sizeof v, 1, fp) == 1);
}

static void
load_uint64 (FILE *fp, uint64_t *v)
{
  assert (fread (v, sizeof *v, 1, fp) == 1);
}

static void
save_unsigned_long_long (FILE *fp, const unsigned long long& v)
{
  save_uint64 (fp, v);
}

static void
load_unsigned_long_long (FILE *fp, unsigned long long *v)
{
  uint64_t t;
  load_uint64 (fp, &t);
  *v = t;
}

static void
save_string (FILE *fp, const std::string& str)
{
  save_int32 (fp, str.size ());
  assert (fwrite (str.c_str (), 1, str.size (), fp) == str.size ());
}

static void
load_string (FILE *fp, std::string *str)
{
  int32_t size;
  load_int32 (fp, &size);
  char buf[size];
  assert ((int) fread (buf, 1, size, fp) == size);
  *str = std::string (buf, size);
}

static void
iprintf (FILE *fp, int i, const char *fmt, ...)
{
  for (int n = 0; n < i; ++ n)
    fprintf (fp, "  ");

  va_list ap;
  va_start (ap, fmt);
  vfprintf (fp, fmt, ap);
  va_end (ap);
}

static void
save_file_location (FILE *fp, const file_location& loc)
{
  save_int32 (fp, loc.line);
  save_int32 (fp, loc.col);
}

static void
load_file_location (FILE *fp, file_location *loc)
{
  load_int32 (fp, &loc->line);
  load_int32 (fp, &loc->col);
}

static void
save_source_location (FILE *fp, const source_location& loc)
{
  save_int32 (fp, loc.fid);
  save_file_location (fp, loc.loc);
}

static void
load_source_location (FILE *fp, source_location *loc)
{
  load_int32 (fp, &loc->fid);
  load_file_location (fp, &loc->loc);
}

static void
save_source_stack (FILE *fp, const source_stack& stack)
{
  save_int32 (fp, stack.locs.size ());
  std::vector<source_location>::const_iterator it;
  for (it = stack.locs.begin ();
       it != stack.locs.end ();
       ++ it)
    save_source_location (fp, *it);
}

static void
load_source_stack (FILE *fp, source_stack *stack)
{
  int32_t size;
  load_int32 (fp, &size);
  for (int i = 0; i < size; ++ i)
    {
      source_location loc;
      load_source_location (fp, &loc);
      stack->locs.push_back (loc);
    }
}

static void
save_expansion_point (FILE *fp, const expansion_point& point)
{
  save_int32 (fp, point.include);
  save_source_location (fp, point.loc);
}

static void
load_expansion_point (FILE *fp, expansion_point *point)
{
  load_int32 (fp, &point->include);
  load_source_location (fp, &point->loc);
}

const jump_to *
context::jump (const unit *unit,
	       const file_location& loc, int expanded_id,
	       file_location *begin) const
{
  jump_from from (loc, 0, expanded_id);
  std::map<jump_from, jump_to>::const_iterator it;
  it = jumps.upper_bound (from);
  if (it == jumps.begin ())
    goto search_surrounding;
  -- it;

  if (expanded_id != 0
      && (it->first.loc != loc || it->first.expanded_id != expanded_id))
    goto search_surrounding;

  if (expanded_id == 0 && it->first.expanded_id != 0)
    {
      it = jumps.upper_bound (jump_from (it->first.loc, 0, 0));
      if (it == jumps.begin ())
	goto search_surrounding;
      -- it;
    }

  if (expanded_id == 0
      && (it->first.loc.line != loc.line
	  || it->first.loc.col + it->first.len <= loc.col))
    goto search_surrounding;

  if (expanded_id == 0 && begin)
    *begin = it->first.loc;
  return &it->second;

search_surrounding:
  if (surrounding)
    return unit->get (surrounding)->jump (unit, loc, expanded_id, begin);
  else
    return NULL;
}

void
context::dump (FILE *fp, int indet, const unit *unit) const
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
	  const gcj::expansion *exp;
	  exp = unit->get_expansion (jump->second.exp);
	  for (tok = exp->tokens.begin ();
	       tok != exp->tokens.end ();
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
      ctx->second.dump (fp, indet + 1, unit);
    }
}

void
context::save (FILE *fp) const
{
  save_int32 (fp, jumps.size ());
  std::map<jump_from, jump_to>::const_iterator jmp;
  for (jmp = jumps.begin (); jmp != jumps.end (); ++ jmp)
    {
      save_file_location (fp, jmp->first.loc);
      save_int32 (fp, jmp->first.len);
      save_int32 (fp, jmp->first.expanded_id);

      save_int32 (fp, jmp->second.include);
      save_int32 (fp, jmp->second.point);
      save_file_location (fp, jmp->second.loc);
      save_int32 (fp, jmp->second.expanded_id);
      save_int32 (fp, jmp->second.exp);
    }

  save_int32 (fp, surrounding);

  save_int32 (fp, expansion_contexts.size ());
  std::map<int, context>::const_iterator ctx;
  for (ctx = expansion_contexts.begin ();
       ctx != expansion_contexts.end ();
       ++ ctx)
    {
      save_int32 (fp, ctx->first);
      ctx->second.save (fp);
    }
}

void
context::load (FILE *fp)
{
  int32_t jmp_size;
  load_int32 (fp, &jmp_size);
  for (int i = 0; i < jmp_size; ++ i)
    {
      jump_from from;
      load_file_location (fp, &from.loc);
      load_int32 (fp, &from.len);
      load_int32 (fp, &from.expanded_id);

      jump_to to;
      load_int32 (fp, &to.include);
      load_int32 (fp, &to.point);
      load_file_location (fp, &to.loc);
      load_int32 (fp, &to.expanded_id);
      load_int32 (fp, &to.exp);

      jumps.insert (std::make_pair (from, to));
    }

  load_int32 (fp, &surrounding);

  int32_t ctx_size;
  load_int32 (fp, &ctx_size);
  for (int i = 0; i < ctx_size; ++ i)
    {
      int id;
      load_int32 (fp, &id);
      expansion_contexts.insert (std::make_pair (id, context ()));
      expansion_contexts.find (id)->second.load (fp);
    }
}

int
unit::file_id (const char *file)
{
  char *full = realpath (file, NULL);
  if (! full)
    return file_map.get (file);
  int id = file_map.get (full);
  free (full);
  return id;
}

void
unit::dump (FILE *fp, int indet) const
{
  for (int i = 1; i <= include_map.size (); ++ i)
    {
      iprintf (fp, indet, "include %d:\n", i);
      std::vector<source_location>::const_iterator loc;
      for (loc = include_map.at (i).locs.begin ();
	   loc != include_map.at (i).locs.end ();
	   ++ loc)
	iprintf (fp, indet + 1, "from %s:%d,%d\n",
		 file_map.at (loc->fid).c_str (),
		 loc->loc.line, loc->loc.col);
    }

  std::map<int, context>::const_iterator ctx;
  for (ctx = contexts.begin (); ctx != contexts.end (); ++ ctx)
    {
      iprintf (fp, indet, "context %d:\n", ctx->first);
      ctx->second.dump (fp, indet + 1, this);
    }
}

void
unit::save (FILE *fp) const
{
  save_string (fp, input);
  save_int32 (fp, input_id);

  save_int32 (fp, contexts.size ());
  std::map<int, context>::const_iterator ctx;
  for (ctx = contexts.begin (); ctx != contexts.end (); ++ ctx)
    {
      save_int32 (fp, ctx->first);
      ctx->second.save (fp);
    }

  save_int32 (fp, expansions.size ());
  std::map<int, expansion>::const_iterator exp;
  for (exp = expansions.begin (); exp != expansions.end (); ++ exp)
    {
      exp->second.map.save (fp, save_unsigned_long_long);

      save_int32 (fp, exp->second.tokens.size ());
      std::vector<expanded_token>::const_iterator tok;
      for (tok = exp->second.tokens.begin ();
	   tok != exp->second.tokens.end ();
	   ++ tok)
	{
	  save_string (fp, tok->token);
	  save_int32 (fp, tok->id);
	}
    }

  file_map.save (fp, save_string);
  include_map.save (fp, save_source_stack);
  point_map.save (fp, save_expansion_point);
}

void
unit::load (FILE *fp)
{
  load_string (fp, &input);
  load_int32 (fp, &input_id);

  int32_t ctx_size;
  load_int32 (fp, &ctx_size);
  for (int i = 0; i < ctx_size; ++ i)
    {
      int32_t id;
      load_int32 (fp, &id);
      contexts.insert (std::make_pair (id, context ()));
      contexts.find (id)->second.load (fp);
    }

  int32_t expansion_size;
  load_int32 (fp, &expansion_size);
  for (int i = 0; i < expansion_size; ++ i)
    {
      expansion *exp = get_expansion (get_expansion ());
      exp->map.load (fp, load_unsigned_long_long);

      int32_t tok_size;
      load_int32 (fp, &tok_size);
      for (int j = 0; j < tok_size; ++ j)
	{
	  expanded_token tok;
	  load_string (fp, &tok.token);
	  load_int32 (fp, &tok.id);
	  exp->tokens.push_back (tok);
	}
    }

  file_map.load (fp, load_string);
  include_map.load (fp, load_source_stack);
  point_map.load (fp, load_expansion_point);
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

void
set::save () const
{
  FILE *fp = fopen (db.c_str (), "wb");
  assert (fp);

  unit_map.save (fp, save_string);

  save_int32 (fp, units.size ());
  std::map<int, unit>::const_iterator it;
  for (it = units.begin (); it != units.end (); ++ it)
    {
      save_int32 (fp, it->first);
      it->second.save (fp);
    }

  fclose (fp);
}

void
set::load ()
{
  FILE *fp = fopen (db.c_str (), "rb");
  if (! fp && errno == ENOENT)
    return;
  assert (fp);

  unit_map.load (fp, load_string);

  int32_t size;
  load_int32 (fp, &size);
  for (int i = 0; i < size; ++ i)
    {
      int id;
      load_int32 (fp, &id);
      units.insert (std::make_pair (id, unit (&log)));
      units.find (id)->second.load (fp);
    }

  fclose (fp);
}

}
