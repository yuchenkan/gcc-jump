#include <string.h>

#include <map>
#include <string>

#include "gcj.hpp"

static bool
to_int (const char *a, int *i)
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
list_elf (gcj::set_usr *set, const char * /* elf */,
	  std::map<std::string, int> *result)
{
  // TODO
  std::map<int, gcj::unit>::iterator it;
  for (it = set->units.begin (); it != set->units.end (); ++ it)
    result->insert(std::make_pair (set->unit_map.at (it->first), it->first));
  return true;
}

struct select_unit_result
{
  int include;
  std::string file;
};

static std::string
get_file (const gcj::unit *unit, int include)
{
  int fid = unit->include_map.at (include).locs.front ().fid;
  return unit->file_map.at (fid);
}

static void
select_unit (gcj::set_usr *set, int unit,
	     select_unit_result *result)
{
  result->include = 0;
  const gcj::unit *u = set->get (unit);
  if (! u || u->input_id == 0) return;

  result->include = u->input_id;
  result->file = get_file (u, u->input_id);
}

struct expand_result
{
  const gcj::expansion *expansion;
  gcj::file_location loc;
};

static void
expand (gcj::set_usr *set,
	int unit, int include, int point, int line, int col,
	expand_result *result)
{
  result->expansion = NULL;
  const gcj::unit *u = set->get (unit);
  if (! u) return;

  const gcj::context *ctx;
  ctx = point == 0
	? u->get (include) : u->get (include, point);

  const gcj::jump_to *to;
  gcj::file_location begin;
  to = ctx->jump (u, gcj::file_location (line, col), 0, &begin);
  if (! to) return;

  result->expansion = u->get_expansion (to->exp);
  result->loc = begin;
}

struct jump_result
{
  const gcj::jump_to *to;
  std::string file;
};

static void
jump (gcj::set_usr *set,
      int unit, int include, int point, int line, int col, int exp,
      jump_result *result)
{
  result->to = NULL;

  const gcj::unit *u = set->get (unit);
  if (! u) return;

  const gcj::context *ctx;
  ctx = point == 0
	? u->get (include) : u->get (include, point);
  if (! ctx) return;

  result->to = ctx->jump (u, gcj::file_location (line, col), exp);
  if (result->to)
    result->file = get_file (u, result->to->include);
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

static int
command (gcj::set_usr *set, const char *cmd,
	 int argc, const char *argv[])
{
  if (strcmp (cmd, "list_elf") == 0)
    {
      if (argc != 1)
	return usage ();

      list_elf_result result;
      if (! list_elf (set, argv[0], &result))
	return 1;

      printf ("[ ");
      list_elf_result::iterator it;
      for (it = result.begin (); it != result.end (); ++ it)
	{
	  if (it != result.begin ()) printf (", ");
	  printf ("[ \"%s\", %d ]",
		  escape (it->first.c_str (), '"').c_str (), it->second);
	}
      printf (" ]");
      return 0;
    }
  else if (strcmp (cmd, "select_unit") == 0)
    {
      int unit;
      if (argc != 1 || ! to_int (argv[0], &unit))
	return usage ();
      select_unit_result result;
      select_unit (set, unit, &result);
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
      expand (set, unit, include, point, line, col,
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
      int unit, include, point, line, col, exp;
      if (argc != 6
	  || ! to_int (argv[0], &unit)
	  || ! to_int (argv[1], &include)
	  || ! to_int (argv[2], &point)
	  || ! to_int (argv[3], &line)
	  || ! to_int (argv[4], &col)
	  || ! to_int (argv[5], &exp))
	return usage ();

      jump_result result;
      jump (set, unit, include, point, line, col, exp,
	    &result);
      if (result.to)
	{
	  fprintf (stderr, "jump to: %d %d %d %d %d %s\n",
		   result.to->include, result.to->point,
		   result.to->loc.line, result.to->loc.col,
		   result.to->expanded_id,
		   result.file.c_str ());

	  printf ("[ \"%s\", ",
		  escape (result.file.c_str (), '"').c_str ());
	  print_vim_context (unit, result.to->include,
			     result.to->point);
	  printf (", ");
	  print_vim_position (result.to->loc.line,
			      result.to->loc.col,
			      result.to->expanded_id);
	  printf (" ]");
	}
      else
	fprintf (stderr, "none\n");

      return 0;
    }
  else
    return 1;
}

int
main (int argc, const char *argv[])
{
  const char *db;
  const char *cmd;

  if (argc < 3)
    return usage ();

  db = argv[1];
  cmd = argv[2];

  int cmd_argc = argc - 3;
  const char **cmd_argv = argv + 3;

  gcj::set_usr set (db);
  return command (&set, cmd, cmd_argc, cmd_argv);
}
