#include <limits.h>
#include <stdlib.h>

#include <sstream>

#include "gcj.hpp"

std::string
escape (const char* str, char c)
{
  std::string ret;
  for (const char* p = str; *p; ++ p)
    {
      if (*p == '\\' || *p == c)
	ret += '\\';
      ret += *p;
    }
  return ret;
}

namespace gcj
{

static std::string
tostr (int v)
{
  std::ostringstream oss;
  oss << v;
  return oss.str ();
}

static std::string
joinpath (const char* first, ...)
{
  std::string p (first);
  va_list ap;
  va_start (ap, first);
  const char* n;
  while (n = va_arg (ap, char*))
    p = p + '/' + n;
  va_end (ap);

  return p;
}

static void
save_string (FILE* fp, const std::string& str)
{
  save_int32 (fp, str.size ());
  assert (fwrite (str.c_str (), 1, str.size (), fp) == str.size ());
}

static void
load_string (FILE* fp, std::string* str)
{
  int size;
  load_int32 (fp, &size);
  char buf[size];
  assert ((int) fread (buf, 1, size, fp) == size);
  *str = std::string (buf, size);
}

static void
iprintf (FILE* fp, int i, const char* fmt, ...)
{
  for (int n = 0; n < i; ++ n)
    fprintf (fp, "  ");

  va_list ap;
  va_start (ap, fmt);
  vfprintf (fp, fmt, ap);
  va_end (ap);
}

static void
save_file_location (FILE* fp, const file_location& loc)
{
  save_int32 (fp, loc.line);
  save_int32 (fp, loc.col);
}

static void
load_file_location (FILE* fp, file_location* loc)
{
  load_int32 (fp, &loc->line);
  load_int32 (fp, &loc->col);
}

static void
save_source_location (FILE* fp, const source_location& loc)
{
  save_int32 (fp, loc.fid);
  save_file_location (fp, loc.loc);
}

static void
load_source_location (FILE* fp, source_location* loc)
{
  load_int32 (fp, &loc->fid);
  load_file_location (fp, &loc->loc);
}

static void
save_source_stack (FILE* fp, const source_stack& stack)
{
  save_int32 (fp, stack.locs.size ());
  std::vector<source_location>::const_iterator it;
  for (it = stack.locs.begin ();
       it != stack.locs.end ();
       ++ it)
    save_source_location (fp, *it);
}

static void
load_source_stack (FILE* fp, source_stack* stack)
{
  int size;
  load_int32 (fp, &size);
  for (int i = 0; i < size; ++ i)
    {
      source_location loc;
      load_source_location (fp, &loc);
      stack->locs.push_back (loc);
    }
}

static void
save_expansion_point (FILE* fp, const expansion_point& point)
{
  save_int32 (fp, point.include);
  save_source_location (fp, point.loc);
}

static void
load_expansion_point (FILE* fp, expansion_point* point)
{
  load_int32 (fp, &point->include);
  load_source_location (fp, &point->loc);
}

void
add_back (int unit_id, int include, int point,
          const jump_from& from,
	  unit* to_unit, const jump_to& to)
{
  context* ctx = to.point
		   ? to_unit->get (to.include, to.point)
		   : to_unit->get (to.include);
  jump_from back_from (to.expanded_id
			 ? jump_from (to.loc, from.len, to.expanded_id)
			 : jump_from (to.loc, from.len));
  jump_to back_to (unit_id, include, point,
		   from.loc, from.expanded_id);
  ctx->back (back_from, back_to);
}

static void print_jump_from (FILE* fp, const jump_from& from);
static void print_jump_to (FILE* fp, const jump_to& to);

// A refers to B, B refers to C, add a backward reference from
// C to A, used e.g. when dealing declarations in header files
void
add_back2 (const context* ctx, const jump_from& from,
	   unit* to_unit, const jump_to& to)
{
  if (! ctx) return;
  const std::set<jump_to>* bks;
  bks = ctx->jump_back (from.loc, from.expanded_id);
  if (! bks) return;
  std::set<jump_to>::const_iterator it;
  for (it = bks->begin (); it != bks->end (); ++ it)
    add_back (it->unit, it->include, it->point,
	      gcj::jump_from (it->loc, from.len,
			      it->expanded_id),
	      to_unit, to);
}

const jump_to*
context::jump (const unit* unit,
	       const file_location& loc, int expanded_id,
	       file_location* begin) const
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

  // TODO: see how to improve this.
  // This is so because currently we both have jumps directly for
  // the tokens in the source code in the format of loc,col,len,0
  // and for the tokens expanded from macro in the format of
  // loc,col,0,exp_id and we sort it by loc,col,exp_id. The key is
  // unique, but creates a wrong order in looking up by a location
  // in the middle of the token in the form loc,col+n,0,0 where n
  // is not 0 but less than len. In such case, the result is
  // pointed to the largest loc,col,0,exp_id where exp_id may not
  // be 0.
  if (expanded_id == 0 && it->first.expanded_id != 0)
    {
      it = jumps.upper_bound (jump_from (it->first.loc, 0, 0));
      if (it == jumps.begin ())
	goto search_surrounding;
      -- it;
    }

  if (expanded_id == 0
      && (it->first.loc.line != loc.line
	  || (it->first.loc.col
	      && it->first.loc.col + it->first.len <= loc.col)))
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

const std::set<jump_to>*
context::jump_back (const file_location& loc, int expanded_id) const
{
  jump_from from (loc, 0, expanded_id);
  std::map<jump_from, std::set<jump_to> >::const_iterator it;
  it = backs.upper_bound (from);
  if (it == backs.begin ())
    return NULL;
  -- it;

  if (expanded_id != 0
      && (it->first.loc != loc || it->first.expanded_id != expanded_id))
    return NULL;

  if (expanded_id == 0 && it->first.expanded_id != 0)
    {
      it = backs.upper_bound (jump_from (it->first.loc, 0, 0));
      if (it == backs.begin ())
	return NULL;
      -- it;
    }

  if (expanded_id == 0
      && (it->first.loc.line != loc.line
	  || (it->first.loc.col
	      && it->first.loc.col + it->first.len <= loc.col)))
    return NULL;

  return &it->second;
}

static void
print_jump_from (FILE* fp, const jump_from& from)
{
  fprintf (fp, "line,col: %d,%d %s:%d",
	   from.loc.line, from.loc.col,
	   ! from.expanded_id || ! from.loc.col ? "len" : "exp",
	   ! from.expanded_id || ! from.loc.col ? from.len : from.expanded_id);
}

static void
print_jump_to (FILE* fp, const jump_to& to)
{
  if (to.point)
    fprintf (fp, "expansion context: %d.%d",
	     to.include, to.point);
  else
    fprintf (fp, "context: %d", to.include);
  fprintf (fp, " line,col: %d,%d",
	   to.loc.line, to.loc.col);
  if (to.expanded_id != 0)
    fprintf (fp, " exp: %d", to.expanded_id);
}

void
context::dump (FILE* fp, int indet, const unit* unit) const
{
  iprintf (fp, indet, "jumps:\n");
  std::map<jump_from, jump_to>::const_iterator jmp;
  for (jmp = jumps.begin (); jmp != jumps.end (); ++ jmp)
    {
      iprintf (fp, indet + 1, "");
      print_jump_from (fp, jmp->first);

      fprintf (fp, " => ");
      print_jump_to (fp, jmp->second);

      fprintf (fp, "\n");

      if (jmp->second.exp)
	{
	  iprintf (fp, indet + 2, "expanded tokens:");

	  std::vector<expanded_token>::const_iterator tok;
	  const gcj::expansion* exp;
	  exp = unit->get_expansion (jmp->second.exp);
	  for (tok = exp->tokens.begin ();
	       tok != exp->tokens.end ();
	       ++ tok)
	    fprintf (fp, " %d \"%s\"",
		     tok->id,
		     escape (tok->token.c_str (), '"').c_str ());
	  fprintf (fp, "\n");
	}
    }

  iprintf (fp, indet, "backs:\n");
  std::map<jump_from, std::set<jump_to> >::const_iterator bak;
  for (bak = backs.begin (); bak != backs.end (); ++ bak)
    {
      iprintf (fp, indet + 1, "");
      print_jump_from (fp, bak->first);
      fprintf (fp, "\n");

      std::set<jump_to>::const_iterator bt;
      for (bt = bak->second.begin (); bt != bak->second.end(); ++ bt)
	{
	  iprintf (fp, indet + 2, "");
	  fprintf (fp, "<= ");
	  print_jump_to (fp, *bt);
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

static void
save_jump_from (FILE* fp, const jump_from& from)
{
  save_file_location (fp, from.loc);
  save_int32 (fp, from.len);
  save_int32 (fp, from.expanded_id);
}

static void
load_jump_from (FILE* fp, jump_from* from)
{
  load_file_location (fp, &from->loc);
  load_int32 (fp, &from->len);
  load_int32 (fp, &from->expanded_id);
}

static void
save_jump_to (FILE* fp, const jump_to& to)
{
  save_int32 (fp, to.unit);
  save_int32 (fp, to.include);
  save_int32 (fp, to.point);
  save_file_location (fp, to.loc);
  save_int32 (fp, to.expanded_id);
  save_int32 (fp, to.exp);
}

static void
load_jump_to (FILE* fp, jump_to* to)
{
  load_int32 (fp, &to->unit);
  load_int32 (fp, &to->include);
  load_int32 (fp, &to->point);
  load_file_location (fp, &to->loc);
  load_int32 (fp, &to->expanded_id);
  load_int32 (fp, &to->exp);
}

void
context::save (FILE* fp) const
{
  save_int32 (fp, jumps.size ());
  std::map<jump_from, jump_to>::const_iterator jmp;
  for (jmp = jumps.begin (); jmp != jumps.end (); ++ jmp)
    {
      save_jump_from (fp, jmp->first);
      save_jump_to (fp, jmp->second);
    }

  save_int32 (fp, backs.size ());
  std::map<jump_from, std::set<jump_to> >::const_iterator bak;
  for (bak = backs.begin (); bak != backs.end (); ++ bak)
    {
      save_jump_from (fp, bak->first);
      save_int32 (fp, bak->second.size());
      std::set<jump_to>::const_iterator bt;
      for (bt = bak->second.begin (); bt != bak->second.end (); ++ bt)
	save_jump_to (fp, *bt);
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
context::load (FILE* fp)
{
  int jmp_size;
  load_int32 (fp, &jmp_size);
  for (int i = 0; i < jmp_size; ++ i)
    {
      jump_from from;
      load_jump_from (fp, &from);

      jump_to to;
      load_jump_to (fp, &to);

      jumps.insert (std::make_pair (from, to));
    }

  int bak_size;
  load_int32 (fp, &bak_size);
  for (int i = 0; i < bak_size; ++ i)
    {
      jump_from from;
      load_jump_from (fp, &from);

      int bt_size;
      load_int32 (fp, &bt_size);
      std::set<jump_to> tos;
      for (int j = 0; j < bt_size; ++ j)
	{
	  jump_to to;
	  load_jump_to (fp, &to);
	  tos.insert(to);
	}

      backs.insert (std::make_pair (from , tos));
    }

  load_int32 (fp, &surrounding);

  int ctx_size;
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
unit::file_id (const char* file)
{
  char* full = realpath (file, NULL);
  if (! full)
    return file_map.get (file);
  int id = file_map.get (full);
  free (full);
  return id;
}

int
unit::include_id (const source_stack& include)
{
  int id = include_map.get (include);

  assert (include.locs.size ());
  int fid = include.locs.front ().fid;
  if (file_includes.find (fid) == file_includes.end ())
    file_includes.insert (std::make_pair (fid, std::set<int> ()));
  file_includes.find (fid)->second.insert (id);

  if (input_id == 0
      && include.locs.size () == 1
      && include.locs.front ().fid == file_id (input.c_str ()))
    input_id = id;

  return id;
}

static void
dump_srcs (FILE* fp, int ident,
	   const std::map<std::string, std::vector<jump_src> >& srcs)
{
  std::map<std::string, std::vector<jump_src> >::const_iterator it;
  for (it = srcs.begin (); it != srcs.end (); ++ it)
    {
      iprintf (fp, ident, "name: %s\n", it->first.c_str ());
      std::vector<jump_src>::const_iterator jt;
      for (jt = it->second.begin (); jt != it->second.end (); ++ jt)
	{
	  iprintf (fp, ident + 1, "include: %d, ", jt->include);
	  print_jump_from (fp, jt->from);
	  fprintf (fp, "\n");
	}
    }
}

static void
dump_tgts (FILE* fp, int ident,
	   const std::map<std::string, jump_tgt>& tgts)
{
  std::map<std::string, jump_tgt>::const_iterator it;
  for (it = tgts.begin (); it != tgts.end (); ++ it)
    {
      iprintf (fp, ident, "name: %s, ", it->first.c_str ());
      assert (it->second.to.include);
      print_jump_to (fp, it->second.to);
      fprintf (fp, ", weak: %d, init: %d\n",
               it->second.weak, it->second.init);
    }
}

void
unit::dump (FILE* fp, int indet) const
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

  std::map<int, std::set<int> >::const_iterator fil;
  for (fil = file_includes.begin ();
       fil != file_includes.end ();
       ++ fil)
    {
      iprintf (fp, indet, "file %s %d:",
	       file_map.at (fil->first).c_str (), fil->first);
      std::set<int>::const_iterator inc;
      for (inc = fil->second.begin ();
	   inc != fil->second.end ();
	   ++ inc)
	fprintf (fp, " %d", *inc);
      fprintf (fp, "\n");
    }

  if (! pub_srcs.empty ())
    {
      iprintf (fp, indet, "jump_pub_srcs: \n");
      dump_srcs (fp, indet + 1, pub_srcs);
    }
  if (! pub_tgts.empty ())
    {
      iprintf (fp, indet, "jump_pub_tgts: \n");
      dump_tgts (fp, indet + 1, pub_tgts);
    }
}
static void
save_srcs (FILE* fp,
	   const std::map<std::string, std::vector<jump_src> >& srcs)
{
  save_int32 (fp, srcs.size ());
  std::map<std::string, std::vector<jump_src> >::const_iterator it;
  for (it = srcs.begin (); it != srcs.end (); ++ it)
    {
      save_string (fp, it->first);
      save_int32 (fp, it->second.size ());
      std::vector<jump_src>::const_iterator jt;
      for (jt = it->second.begin (); jt != it->second.end (); ++ jt)
	{
	  save_int32 (fp, jt->include);
	  save_jump_from (fp, jt->from);
	}
    }
}

static void
load_srcs (FILE* fp,
	   std::map<std::string, std::vector<jump_src> >* srcs)
{
  int map_size;
  load_int32 (fp, &map_size);
  for (int i = 0; i < map_size; ++ i)
    {
      std::string name;
      load_string (fp, &name);
      int vec_size;
      load_int32 (fp, &vec_size);
      std::vector<jump_src> vec;
      for (int j = 0; j < vec_size; ++ j)
	{
	  int include;
	  load_int32 (fp, &include);
	  jump_from from;
	  load_jump_from (fp, &from);
	  vec.push_back (jump_src (include, from));
	}
      srcs->insert (std::make_pair (name, vec));
    }
}

static void
save_tgts (FILE* fp, const std::map<std::string, jump_tgt>& tgts)
{
  save_int32 (fp, tgts.size ());
  std::map<std::string, jump_tgt>::const_iterator it;
  for (it = tgts.begin (); it != tgts.end (); ++ it)
    {
      save_string (fp, it->first);
      save_jump_to (fp, it->second.to);
      save_int32 (fp, it->second.weak);
      save_int32 (fp, it->second.init);
    }
}

static void
load_tgts (FILE* fp, std::map<std::string, jump_tgt>* tgts)
{
  int map_size;
  load_int32 (fp, &map_size);
  for (int i = 0; i < map_size; ++ i)
    {
      std::string name;
      load_string (fp, &name);
      jump_to to;
      load_jump_to (fp, &to);
      int weak;
      load_int32 (fp, &weak);
      int init;
      load_int32 (fp, &init);
      tgts->insert (std::make_pair (name,
                                    jump_tgt (to,
                                              (bool) weak,
                                              (bool) init)));
    }
}

static void
save_int32r (FILE* fp, const int& v)
{
  save_int32 (fp, v);
}

void
unit::save (const std::string& path) const
{
  FILE* fp = fopen (path.c_str (), "wb");
  assert (fp);

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
      exp->second.map.save (fp, save_int32r);

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

  save_int32 (fp, file_includes.size ());
  std::map<int, std::set<int> >::const_iterator fil;
  for (fil = file_includes.begin ();
       fil != file_includes.end ();
       ++ fil)
    {
      save_int32 (fp, fil->first);
      save_int32 (fp, fil->second.size ());
      std::set<int>::const_iterator inc;
      for (inc = fil->second.begin ();
	   inc != fil->second.end ();
	   ++ inc)
	save_int32 (fp, *inc);
    }

  save_srcs (fp, pub_srcs);
  save_tgts (fp, pub_tgts);

  fclose (fp);
}

void
unit::load (const std::string& path)
{
  FILE* fp = fopen (path.c_str (), "rb");
  assert (fp);

  load_string (fp, &input);
  load_int32 (fp, &input_id);

  int ctx_size;
  load_int32 (fp, &ctx_size);
  for (int i = 0; i < ctx_size; ++ i)
    {
      int id;
      load_int32 (fp, &id);
      contexts.insert (std::make_pair (id, context ()));
      contexts.find (id)->second.load (fp);
    }

  int expansion_size;
  load_int32 (fp, &expansion_size);
  for (int i = 0; i < expansion_size; ++ i)
    {
      expansion* exp = get_expansion (get_expansion ());
      exp->map.load (fp, load_int32);

      int tok_size;
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

  int fil_size;
  load_int32 (fp, &fil_size);
  for (int i = 0; i < fil_size; ++ i)
    {
      int fid;
      load_int32 (fp, &fid);
      int inc_size;
      load_int32 (fp, &inc_size);
      std::set<int> includes;
      for (int j = 0; j < inc_size; ++ j)
	{
	  int inc_id;
	  load_int32 (fp, &inc_id);
	  includes.insert (inc_id);
	}
      file_includes.insert (std::make_pair (fid, includes));
    }

  load_srcs (fp, &pub_srcs);
  load_tgts (fp, &pub_tgts);

  fclose (fp);
}

static std::string
index_path (const std::string& db)
{
  return joinpath (db.c_str (), "index", NULL);
}

static std::string
unit_path (const std::string& db, int id)
{
  return joinpath (db.c_str(), "units", tostr (id).c_str (), NULL);
}

static std::string
unit_path (const std::string& db, int ld, int id)
{
  return joinpath (db.c_str(), "units",
		   (tostr (ld) + '.' + tostr (id)).c_str (), NULL);
}

static std::string
files_path (const std::string& db, int ld)
{
  return joinpath (db.c_str(), "files", tostr (ld).c_str (), NULL);
}

void
set_data::save (const std::string& path) const
{
  FILE* fp = fopen (path.c_str (), "wb");
  assert (fp);

  unit_map.save (fp, save_string);
  ld_map.save (fp, save_string);

  save_int32 (fp, ld_units.size ());
  std::map<int, std::set<int> >::const_iterator it;
  for (it = ld_units.begin (); it != ld_units.end (); ++ it)
    {
      save_int32 (fp, it->first);
      save_int32 (fp, it->second.size ());
      std::set<int>::const_iterator jt;
      for (jt = it->second.begin (); jt != it->second.end (); ++ jt)
	save_int32 (fp, *jt);
    }

  fclose (fp);
}

void
set_data::load (const std::string& path)
{
  FILE* fp = fopen (path.c_str (), "rb");
  if (! fp)
    return;

  unit_map.load (fp, load_string);
  ld_map.load (fp, load_string);

  int size;
  load_int32 (fp, &size);
  for (int i = 0; i < size; ++ i)
    {
      int ld;
      load_int32 (fp, &ld);
      ld_units.insert (std::make_pair (ld, std::set<int> ()));
      int unit_size;
      load_int32 (fp, &unit_size);
      for (int j = 0; j < unit_size; ++ j)
	{
	  int id;
	  load_int32 (fp, &id);
	  ld_units.find (ld)->second.insert (id);
	}
    }

  fclose (fp);
}

set::set (const char* db, int flags)
  : log (stderr, flags & SF_TRACE),
    db (db), dump (flags & SF_DUMP), cur_id (0), cur_data (NULL)
{
  trace ("load from %s\n", db);
  data.load (index_path (db));
}

set::~set ()
{
  if (cur_id)
    save_current_unit ();

  trace ("save to %s\n", db.c_str ());
  data.save (index_path (db));
}

void
set::next (const std::string& args, const std::string& input)
{
  if (cur_id)
    save_current_unit ();

  cur_id = data.unit_map.get (args);
  cur = unit (&log, input);
}

void
set::save_current_unit ()
{
  if (dump)
    {
      fprintf (stderr, "unit %s:\n", data.unit_map.at (cur_id).c_str ());
      cur.dump (stderr, 1);
    }
  cur.save (unit_path (db, cur_id));

  std::map<int, std::set<int> >::iterator it;
  std::vector<int> rm;
  for (it = data.ld_units.begin (); it != data.ld_units.end (); ++ it)
    if (it->second.find (cur_id) != it->second.end ())
      rm.push_back (it->first);
  std::vector<int>::iterator jt;
  for (jt = rm.begin (); jt != rm.end (); ++ jt)
    data.ld_units.erase (*jt);
}

static void
save_unit_fid (FILE* fp, const unit_fid& fid)
{
  save_int32 (fp, fid.unit);
  save_int32 (fp, fid.fid);
}

static void
load_unit_fid (FILE* fp, unit_fid* fid)
{
  load_int32 (fp, &fid->unit);
  load_int32 (fp, &fid->fid);
}

void
file_set::save (const std::string& path) const
{
  FILE* fp = fopen (path.c_str (), "wb");
  assert (fp);

  file_map.save (fp, save_string);

  save_int32 (fp, files.size ());
  std::map<unit_fid, int>::const_iterator file;
  for (file = files.begin (); file != files.end (); ++ file)
    {
      save_unit_fid (fp, file->first);
      save_int32 (fp, file->second);
    }

  save_int32 (fp, file_units.size ());
  std::map<int, std::set<unit_fid> >::const_iterator unit;
  for (unit = file_units.begin (); unit != file_units.end (); ++ unit)
    {
       save_int32 (fp, unit->first);
       save_int32 (fp, unit->second.size ());
       std::set<unit_fid>::const_iterator fid;
       for (fid = unit->second.begin ();
	    fid != unit->second.end ();
	    ++ fid)
	 save_unit_fid (fp, *fid);
    }

  fclose (fp);
}

void
file_set::load (const std::string& path)
{
  FILE* fp = fopen (path.c_str (), "rb");
  assert (fp);

  file_map.load (fp, load_string);

  int file_size;
  load_int32 (fp, &file_size);
  for (int i = 0; i < file_size; ++ i)
    {
      unit_fid ufid;
      load_unit_fid (fp, &ufid);
      int fid;
      load_int32 (fp, &fid);
      files.insert (std::make_pair (ufid, fid));
    }

  int file_unit_size;
  load_int32 (fp, &file_unit_size);
  for (int i = 0; i < file_unit_size; ++ i)
    {
      int fid;
      load_int32 (fp, &fid);
      int unit_size;
      load_int32 (fp, &unit_size);
      std::set<unit_fid> units;
      for (int j = 0; j < unit_size; ++ j)
	{
	  unit_fid ufid;
	  load_unit_fid (fp, &ufid);
	  units.insert (ufid);
	}
      file_units.insert (std::make_pair (fid, units));
    }

  fclose (fp);
}

set_usr::set_usr (const std::string& db)
  : db (db)
{
  data.load (index_path (db));
}

void
set_usr::build_files (int ld)
{
  assert (ld_files.find (ld) == ld_files.end ());
  ld_files.insert (std::make_pair (ld, file_set ()));
  file_set* fset = &ld_files.find (ld)->second;

  assert (data.ld_units.find (ld) != data.ld_units.end ());
  const std::set<int>* units = &data.ld_units.find (ld)->second;
  std::set<int>::const_iterator it;
  for (it = units->begin (); it != units->end (); ++ it)
    {
      const unit* unit = get (*it);
      std::map<int, std::set<int> >::const_iterator jt;
      for (jt = unit->file_includes.begin ();
	   jt != unit->file_includes.end (); ++ jt)
	{
	  int fid = jt->first;
	  const std::string file = unit->file_map.at (fid);
	  char* full = realpath (file.c_str(), NULL);
	  if (! full)
	    continue;

	  int set_fid = fset->file_map.get (std::string (full));
	  free (full);

	  unit_fid ufid (*it, fid);
	  fset->files.insert (std::make_pair (ufid, set_fid));
	  if (fset->file_units.find (set_fid) == fset->file_units.end ())
	    fset->file_units.insert (std::make_pair (set_fid,
						     std::set<unit_fid> ()));
	  fset->file_units.find (set_fid)->second.insert (ufid);
	}
    }

  fset->save (files_path (db, ld));
}

static void
add_jump_src (std::map<std::string, std::vector<std::pair<int, jump_src> > >* srcs,
              const std::string name, int unit, const std::vector<jump_src>& src)
{
  if (srcs->find (name) == srcs->end ())
    srcs->insert (std::make_pair (name,
				  std::vector<std::pair<int, jump_src> > ()));

  std::vector<jump_src>::const_iterator it;
  for (it = src.begin (); it != src.end (); ++ it)
    srcs->find (name)->second.push_back (std::make_pair (unit, *it));
}

static void
add_jump_src (std::map<std::string, std::vector<std::pair<int, jump_src> > >* srcs,
              const std::string name, int unit, const jump_src& src)
{
  if (srcs->find (name) == srcs->end ())
    srcs->insert (std::make_pair (name,
				  std::vector<std::pair<int, jump_src> > ()));

  srcs->find (name)->second.push_back (std::make_pair (unit, src));
}

int
set_usr::get_ld (const char* name, const std::set<int>& units)
{
  char* full = realpath (name, NULL);
  if (! full)
    return 0;
  int id = data.ld_map.get (std::string (full));
  free (full);
  if (data.ld_units.find (id) == data.ld_units.end ())
    {
      std::map<std::string, std::vector<std::pair<int, jump_src> > > srcs;
      std::map<std::string, jump_tgt> tgts;

      // TODO this is really slow
      std::set<int>::const_iterator it;
      for (it = units.begin (); it != units.end (); ++ it)
	{
	  const unit* unit = get (*it);
	  std::map<std::string, std::vector<jump_src> >::const_iterator jt;
	  for (jt = unit->pub_srcs.begin ();
	       jt != unit->pub_srcs.end (); ++ jt)
	    add_jump_src (&srcs, jt->first, *it, jt->second);

	  std::map<std::string, jump_tgt>::const_iterator lt;
	  for (lt = unit->pub_tgts.begin ();
	       lt != unit->pub_tgts.end (); ++ lt)
	    {
	      if (tgts.find (lt->first) == tgts.end ())
		tgts.insert (std::make_pair (lt->first,
					     lt->second));
	      else if (tgts.find (lt->first)->second.weak
		       && ! lt->second.weak)
		tgts.find (lt->first)->second = lt->second;
              else if (tgts.find (lt->first)->second.init)
                add_jump_src (&srcs, lt->first, *it,
                              jump_src (lt->second.to.include,
                                        jump_from (lt->second.to.loc,
                                                   lt->first.length (),
                                                   lt->second.to.expanded_id)));
              else
                {
                  const jump_to& old_to = tgts.find (lt->first)->second.to;
                  add_jump_src (&srcs, lt->first, old_to.unit,
                                jump_src (old_to.include,
                                          jump_from (old_to.loc,
                                                     lt->first.length (),
                                                     old_to.expanded_id)));
                  tgts.find (lt->first)->second = lt->second;
                }
	    }
	}

      std::map<int, unit> ld_units;
      std::map<std::string,
	       std::vector<std::pair<int, jump_src> > >::iterator mt;
      for (mt = srcs.begin (); mt != srcs.end (); ++ mt)
	{
	  if (tgts.find (mt->first) == tgts.end ())
	    continue;

	  const gcj::jump_to& to = tgts.find (mt->first)->second.to;
	  std::vector<std::pair<int, jump_src> >::iterator nt;
	  for (nt = mt->second.begin (); nt != mt->second.end (); ++ nt)
	    {
	      int unit_id = nt->first;
	      if (ld_units.find (unit_id) == ld_units.end ())
		ld_units.insert (std::make_pair (unit_id, unit ()));

	      unit* from_unit = &ld_units.find (unit_id)->second;
	      context* ctx = from_unit->get (nt->second.include);
	      // Cross link the declarations to the definitions
	      ctx->add (nt->second.from, to);

	      if (ld_units.find (to.unit) == ld_units.end ())
		ld_units.insert (std::make_pair (to.unit, unit ()));

              // add_back (unit_id, nt->second.include, 0, nt->second.from,
              //           &ld_units.find (to.unit)->second, to);
	      add_back2 (get (unit_id)->get (nt->second.include),
			 nt->second.from,
			 &ld_units.find (to.unit)->second, to);
	    }
	}

      std::map<int, unit>::iterator ot;
      for (ot = ld_units.begin (); ot != ld_units.end (); ++ ot)
	ot->second.save (unit_path (db, id, ot->first));

      // Create an empty file for those have no linkage data
      std::set<int>::const_iterator pt;
      for (pt = units.begin (); pt != units.end (); ++ pt)
        if (ld_units.find (*pt) == ld_units.end ())
          unit ().save (unit_path (db, id, *pt));

      data.ld_units.insert (std::make_pair (id, units));
      build_files (id);
      data.save (index_path (db));
    }
  else
    assert (data.ld_units.find (id)->second == units);
  return id;
}

const unit*
set_usr::get (int id)
{
  if (id == 0 || id > data.unit_map.size ())
    return NULL;

  if (units.find (id) == units.end ())
    {
      units.insert (std::make_pair (id, unit (NULL)));
      units.find (id)->second.load (unit_path (db, id));
    }

  return &units.find (id)->second;
}

bool
set_usr::check_ld (int ld)
{
  return ld != 0
	 && ld <= data.ld_map.size()
	 // This is possible if it's erased by rebuilding unit,
	 // if so, rebuild it by calling get_ld with unit set
	 && data.ld_units.find (ld) != data.ld_units.end ();
}

const unit*
set_usr::get (int ld, int id)
{
  if (! check_ld (ld)
      || data.ld_units.find (ld)->second.find (id)
	 == data.ld_units.find (ld)->second.end ())
    return NULL;

  if (ld_units.find (ld) == ld_units.end ())
    ld_units.insert (std::make_pair (ld, std::map<int, unit> ()));

  if (ld_units.find (ld)->second.find (id)
      == ld_units.find (ld)->second.end ())
    {
      ld_units.find (ld)->second.insert (std::make_pair (id, unit (NULL)));
      ld_units.find (ld)->second.find (id)->second.load (unit_path (db, ld, id));
    }

  return &ld_units.find (ld)->second.find (id)->second;
}

const file_set*
set_usr::get_file_set (int ld)
{
  if (! check_ld (ld)) return NULL;

  if (ld_files.find (ld) == ld_files.end ())
    {
      ld_files.insert (std::make_pair (ld, file_set ()));
      ld_files.find (ld)->second.load (files_path (db, ld));
    }
  return &ld_files.find (ld)->second;
}

}
