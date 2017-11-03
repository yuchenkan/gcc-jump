#include <string.h>

#include <map>
#include <string>

#include "gcj.hpp"
#include "elf.hpp"

static bool
to_int (const char* a, int* i)
{
  return sscanf (a, "%d", i) == 1;
}

static int
usage ()
{
  fprintf (stderr, "db command\n");
  return 1;
}

typedef std::map<std::string, int> list_elf_result;

static bool
read_elf (const char* name,
	  std::set<int>* unit_ids)
{
  elf_reader elf;
  std::string err;
  std::string sec;

  err = elf.open (name);
  if (! err.empty ())
    goto error_out;

  err = elf.read_section (".GCJ.plugin", &sec);
  if (! err.empty ())
    goto error_out;

  if (sec.size () % 4)
    {
      fprintf (stderr, "invalid gcj section length\n");
      return false;
    }

  for (unsigned int i = 0; i < sec.size (); i += 4)
    unit_ids->insert (elf.get_int (sec.c_str () + i, 4));

  elf.close ();
  return true;

error_out:
  elf.close ();
  fprintf (stderr, "%s\n", err.c_str ());
  return false;
}

static bool
list_elf (gcj::set_usr* set, const char* elf,
	  std::map<std::string, int>* result)
{
  if (elf)
    {
      std::set<int> unit_ids;
      if (! read_elf (elf, &unit_ids))
	return false;
      std::set<int>::iterator it;
      for (it = unit_ids.begin (); it != unit_ids.end (); ++ it)
	if (set->data.unit_map.contains (*it))
	  result->insert (std::make_pair (set->data.unit_map.at (*it), *it));
    }
  else
    {
      for (int id = 1; id <= set->data.unit_map.size (); ++ id)
	result->insert (std::make_pair (set->data.unit_map.at (id), id));
    }
  return true;
}

struct select_unit_result
{
  int include;
  std::string file;
};

static int
get_fid (const gcj::unit* unit, int include)
{
  return unit->include_map.at (include).locs.front ().fid;
}

static std::string
get_file (const gcj::unit* unit, int include)
{
  return unit->file_map.at (get_fid (unit, include));
}

static void
select_unit (gcj::set_usr* set, int unit,
	     select_unit_result* result)
{
  result->include = 0;
  const gcj::unit* u = set->get (unit);
  if (! u || u->input_id == 0) return;

  result->include = u->input_id;
  result->file = get_file (u, u->input_id);
}

struct expand_result
{
  const gcj::expansion* expansion;
  gcj::file_location loc;
};

static void
expand (gcj::set_usr* set,
	int unit, int include, int point, int line, int col,
	expand_result* result)
{
  result->expansion = NULL;
  const gcj::unit* u = set->get (unit);
  if (! u) return;

  const gcj::context* ctx;
  ctx = point == 0
	? u->get (include) : u->get (include, point);

  const gcj::jump_to* to;
  gcj::file_location begin;
  to = ctx->jump (u, gcj::file_location (line, col), 0, &begin);
  if (! to) return;

  result->expansion = u->get_expansion (to->exp);
  result->loc = begin;
}

struct jump_result
{
  const gcj::jump_to* to;
  std::string file;

  jump_result ()
    : to (NULL)
  {
  }

  jump_result (const gcj::jump_to* to, const std::string& file)
    : to (to), file (file)
  {
  }
};

static bool
unit_jump (const gcj::unit* unit, gcj::set_usr* set,
	   int include, int point, int line, int col, int exp,
	   jump_result* result)
{
  if (! unit) return false;

  const gcj::context* ctx;
  ctx = point == 0
	? unit->get (include) : unit->get (include, point);
  if (! ctx) return false;

  result->to = ctx->jump (unit,
			  gcj::file_location (line, col), exp, NULL);
  if (result->to)
    result->file = get_file (set->get (result->to->unit),
			     result->to->include);
  return result->to;
}

static void
jump (gcj::set_usr* set,
      int ld, int unit, int include, int point, int line, int col, int exp,
      jump_result* result)
{
  result->to = NULL;

  if (unit_jump (set->get (unit), set,
		 include, point, line, col, exp, result))
    return;

  if (ld)
    unit_jump (set->get (ld, unit), set,
	       include, point, line, col, exp, result);
}

static void
context_refer (const gcj::context* ctx,
	       gcj::set_usr* set,
	       const gcj::file_location& loc, int exp,
	       std::vector<jump_result>* results)
{
  const std::set<gcj::jump_to>* backs;
  backs = ctx->jump_back (loc, exp);
  if (! backs) return;

  std::set<gcj::jump_to>::const_iterator it;
  for (it = backs->begin (); it != backs->end (); ++ it)
    results->push_back (jump_result (&(*it),
				     get_file (set->get (it->unit),
					       it->include)));
}

static void
unit_refer (const gcj::unit* unit,
	    const std::map<int, std::set<int> >* file_includes,
	    gcj::set_usr* set,
	    int fid, int line, int col, int exp,
	    std::vector<jump_result>* results)
{
  if (! unit
      || file_includes->find (fid) == file_includes->end ())
    return;

  gcj::file_location loc (line, col);

  const std::set<int>* incs = &file_includes->find (fid)->second;
  std::set<int>::const_iterator it;
  for (it = incs->begin (); it != incs->end (); ++ it)
    {
      const gcj::context* ctx = unit->get (*it);
      if (! ctx) continue;

      context_refer (ctx, set, loc, exp, results);

      std::map<int, gcj::context>::const_iterator jt;
      for (jt = ctx->expansion_contexts.begin ();
	   jt != ctx->expansion_contexts.end ();
	   ++jt)
	context_refer (&jt->second, set, loc, exp, results);
    }
}

static void
refer (gcj::set_usr* set,
       int ld, int unit, int include, int line, int col, int exp,
       std::vector<jump_result>* results)
{
  const gcj::unit* pos_unit = set->get (unit);
  int pos_fid = get_fid (pos_unit, include);

  gcj::unit_fid ufid (unit, pos_fid);

  std::set<gcj::unit_fid> unit_fids;
  unit_fids.insert (ufid);

  const gcj::file_set* file_set = set->get_file_set (ld);
  if (file_set
      && file_set->files.find (ufid) != file_set->files.end ())
    {
      int fid = file_set->files.find (ufid)->second;
      assert (file_set->file_units.find (fid)
	      != file_set->file_units.end ()); 

      const std::set<gcj::unit_fid>* ufids;
      ufids = &file_set->file_units.find (fid)->second;
      unit_fids.insert (ufids->begin (), ufids->end());
    }

  std::set<gcj::unit_fid>::iterator it;
  for (it = unit_fids.begin (); it != unit_fids.end (); ++ it)
    {
      const gcj::unit* base = set->get (it->unit);
      unit_refer (base, &base->file_includes,
		  set, it->fid, line, col, exp,
		  results);
      unit_refer (set->get (ld, it->unit), &base->file_includes,
		  set, it->fid, line, col, exp,
		  results);
    }
}

static void
print_vim_context (int unit, int include, int point)
{
  printf ("{ \"unit\": %d, \"include\": %d, \"point\": %d }",
	  unit, include, point);
}

static void
print_vim_position (int line, int col)
{
  printf ("{ \"line\": %d, \"col\": %d }", line, col);
}

static void
print_vim_position (int line, int col, int expid)
{
  printf ("{ \"line\": %d, \"col\": %d, \"expid\": %d }",
	  line, col, expid);
}

static void
print_vim_jump_result (const jump_result& result)
{
  printf ("[ \"%s\", ",
	  escape (result.file.c_str (), '"').c_str ());
  print_vim_context (result.to->unit, result.to->include,
		     result.to->point);
  printf (", ");
  print_vim_position (result.to->loc.line,
		      result.to->loc.col,
		      result.to->expanded_id);
  printf (" ]");
}

static int
command (const char* db, const char* cmd,
	 int argc, const char* argv[])
{
  gcj::set_usr set (db);
  if (strcmp (cmd, "list_elf") == 0)
    {
      if (argc > 1)
	return usage ();

      list_elf_result result;
      if (! list_elf (&set, argc == 0 ? NULL : argv[0], &result))
	return 1;

      int ld = 0;
      if (argc != 0)
	{
	  std::set<int> units;
	  list_elf_result::iterator it;
	  for (it = result.begin (); it != result.end (); ++ it)
	    units.insert (it->second);
	  ld = set.get_ld (argv[0], units);
	  if (ld == 0)
	    {
	      fprintf (stderr, "file not found %s\n", argv[0]);
	      return 1;
	    }
	}

      printf ("[ %d, [ ", ld);
      list_elf_result::iterator it;
      for (it = result.begin (); it != result.end (); ++ it)
	{
	  if (it != result.begin ()) printf (", ");
	  printf ("[ \"%s\", %d ]",
		  escape (it->first.c_str (), '"').c_str (), it->second);
	}
      printf (" ] ]");
      return 0;
    }
  else if (strcmp (cmd, "select_unit") == 0)
    {
      int unit;
      if (argc != 1 || ! to_int (argv[0], &unit))
	return usage ();
      select_unit_result result;
      select_unit (&set, unit, &result);
      if (result.include)
	{
	  fprintf (stderr, "selected: %d %s\n",
		   result.include, result.file.c_str ());
	  printf ("[ \"%s\", ",
		  escape (result.file.c_str (), '"').c_str ());
	  print_vim_context (unit, result.include, 0);
	  printf (" ]");
	}
      else
	fprintf (stderr, "none\n");

      return 0;
    }
  else if (strcmp (cmd, "expand") == 0)
    {
      int unit, include, point, line, col;
      if (argc != 5
	  || ! to_int (argv[0], &unit)
	  || ! to_int (argv[1], &include)
	  || ! to_int (argv[2], &point)
	  || ! to_int (argv[3], &line)
	  || ! to_int (argv[4], &col))
	return usage ();

      expand_result result;
      expand (&set, unit, include, point, line, col,
	      &result);

      if (result.expansion)
	{
	  printf ("[ ");
	  print_vim_position (result.loc.line, result.loc.col);
	  printf (", [ ");
	  std::vector<gcj::expanded_token>::const_iterator it;
	  for (it = result.expansion->tokens.begin ();
	       it != result.expansion->tokens.end ();
	       ++ it)
	    {
	      fprintf (stderr, "token: %d %s\n",
		       it->id, it->token.c_str ());

	      if (it != result.expansion->tokens.begin ())
		printf (", ");
	      printf ("[ \"%s\", %d ]",
		      escape (it->token.c_str (), '"').c_str (),
		      it->id);
	    }
	  printf ("] ]");
	}
      else
	fprintf (stderr, "none\n");

      return 0;
    }
  else if (strcmp (cmd, "jump") == 0)
    {
      int ld, unit, include, point, line, col, exp;
      if (argc != 7
	  || ! to_int (argv[0], &ld)
	  || ! to_int (argv[1], &unit)
	  || ! to_int (argv[2], &include)
	  || ! to_int (argv[3], &point)
	  || ! to_int (argv[4], &line)
	  || ! to_int (argv[5], &col)
	  || ! to_int (argv[6], &exp))
	return usage ();

      jump_result result;
      jump (&set, ld, unit, include, point, line, col, exp,
	    &result);
      if (result.to)
	{
	  fprintf (stderr, "jump to: %d %d %d %d %d %s\n",
		   result.to->include, result.to->point,
		   result.to->loc.line, result.to->loc.col,
		   result.to->expanded_id,
		   result.file.c_str ());

	  print_vim_jump_result (result);
	}
      else
	fprintf (stderr, "none\n");

      return 0;
    }
  else if (strcmp (cmd, "refer") == 0)
    {
      int ld, unit, include, line, col, exp;
      if (argc != 6
	  || ! to_int (argv[0], &ld)
	  || ! to_int (argv[1], &unit)
	  || ! to_int (argv[2], &include)
	  || ! to_int (argv[3], &line)
	  || ! to_int (argv[4], &col)
	  || ! to_int (argv[5], &exp))
	return usage ();

      std::vector<jump_result> results;
      refer (&set, ld, unit, include, line, col, exp, &results);
      std::vector<jump_result>::iterator it;
      printf ("[ ");
      for (it = results.begin (); it != results.end (); ++ it)
	{
	  fprintf (stderr, "refered by: %d %d %d %d %d %s\n",
		   it->to->include, it->to->point,
		   it->to->loc.line, it->to->loc.col,
		   it->to->expanded_id,
		   it->file.c_str ());

	  if (it != results.begin ()) printf (", ");
	  print_vim_jump_result (*it);
	}
      printf (" ]");

      return 0;
    }
  else
    return 1;
}

int
main (int argc, const char* argv[])
{
  const char* db;
  const char* cmd;

  if (argc < 3)
    return usage ();

  db = argv[1];
  cmd = argv[2];

  int cmd_argc = argc - 3;
  const char** cmd_argv = argv + 3;

  return command (db, cmd, cmd_argc, cmd_argv);
}
