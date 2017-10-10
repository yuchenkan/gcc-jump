#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

#include <string>
#include <map>
#include <set>
#include <vector>
#include <list>

std::string escape(const char *str, char c);

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

  void
  vwarning (const char *fmt, va_list ap)
  {
    if (enabled)
      vfprintf (fp, fmt, ap);
  }

  FILE *fp;
  bool enabled;
};

namespace gcj
{

inline void
save_int32 (FILE *fp, int v)
{
  int32_t t = v;
  assert (fwrite (&t, sizeof t, 1, fp) == 1);
}

inline void
load_int32 (FILE *fp, int *v)
{
  int32_t t;
  assert (fread (&t, sizeof t, 1, fp) == 1);
  *v = t;
}

template <typename type>
class id_map
{
public:
  id_map ()
    : cur (0)
  {
  }
  id_map (const id_map& map)
    : cur (map.cur), map (map.map), vec (map.vec)
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
      vec.push_back (&map.find (key)->first);
    }
    return map.find (key)->second;
  }

  int
  size () const
  {
    return cur;
  }

  bool
  contains (int id) const
  {
    return (int) vec.size () >= id;
  }

  const type&
  at (int id) const
  {
    assert ((int) vec.size () >= id);
    return *vec.at (id - 1);
  }

  void
  save (FILE *fp, void (* sv)(FILE *, const type&)) const
  {
    save_int32 (fp, cur);
    typename std::vector<const type *>::const_iterator it;
    for (it = vec.begin (); it != vec.end (); ++ it)
      sv (fp, **it);
  }

  void
  load (FILE *fp, void (* ld)(FILE *, type *))
  {
    assert (cur == 0);

    load_int32 (fp, &cur);
    for (int i = 1; i <= cur; ++ i)
      {
	type v;
	ld (fp, &v);
	map.insert (std::make_pair (v, i));
	vec.push_back (&map.find (v)->first);
      }
  }

private:
  int cur;
  std::map<type, int> map;
  std::vector<const type *> vec;
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

  bool
  operator== (const file_location &rhs) const
  {
    return line == rhs.line && col == rhs.col;
  }

  int line;
  int col;
};

struct source_location
{
  source_location ()
    : fid (0)
  {
  }

  source_location (const source_location& loc)
    : fid (loc.fid), loc (loc.loc)
  {
  }

  source_location (int fid)
    : fid (fid)
  {
  }

  source_location (int fid, const file_location& loc)
    : fid (fid), loc (loc)
  {
  }

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
  expansion_point ()
    : include (0)
  {
  }

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
  expanded_token ()
    : id (0)
  {
  }

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
  add (const std::string& token, int exp)
  {
    tokens.push_back (expanded_token (token, map.get (exp)));
  }

  int
  id (int exp) const
  {
    return map.get (exp);
  }

  id_map<int> map;
  std::vector<expanded_token> tokens;
};

struct context;

struct jump_to
{
  jump_to ()
    : unit (0), include (0), point (0), expanded_id (0), exp (0)
  {
  }

  jump_to (const jump_to& to)
    : unit (to.unit), include (to.include), point (to.point),
      loc (to.loc), expanded_id (to.expanded_id),
      exp (to.exp)
  {
  }

  jump_to (int unit, int include, int point,
	   const file_location& loc)
    : unit (unit), include (include), point (point), loc (loc),
      expanded_id (0), exp (0)
  {
  }

  jump_to (int unit, int include, int point,
	   const file_location& loc, int, int exp)
    : unit (unit), include (include), point (point), loc (loc),
      expanded_id (0), exp (exp)
  {
  }

  jump_to (int unit, int include, int point,
	   const file_location& loc, int expanded_id)
    : unit (unit), include (include), point (point), loc (loc),
      expanded_id (expanded_id), exp (0)
  {
  }

  bool
  operator== (const jump_to &rhs) const
  {
    return unit == rhs.unit
	   && include == rhs.include && point == rhs.point
	   && loc == rhs.loc && expanded_id == rhs.expanded_id
	   && exp == rhs.exp;
  }

  int unit;

  int include;
  int point;

  file_location loc;
  int expanded_id;

  int exp;
};

struct context
{
  context ()
    : surrounding (0)
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
  expansion (int include, int point)
  {
    std::pair<std::map<int, context>::iterator, bool> pair;
    pair = expansion_contexts.insert (std::make_pair (point, context ()));
    if (pair.second)
      pair.first->second.surrounding = include;
    return &pair.first->second;
  }

  void
  add (const jump_from& from, const jump_to& to)
  {
    //assert (jumps.insert (std::make_pair (from, to)).second);
    if (jumps.find (from) == jumps.end ())
      jumps.insert (std::make_pair (from, to));
    else
      {
	std::map<jump_from, jump_to>::iterator it = jumps.find (from);
        assert (it->second == to);
      }
  }

  const jump_to *jump (const unit *, const file_location&, int,
		       file_location *) const;

  jump_to *
  jump (const unit *unit, const file_location& loc, int expanded_id)
  {
    return (jump_to *) ((const context *) this)->jump (unit, loc,
						       expanded_id,
						       NULL);
  }

  void dump (FILE *, int, const unit *) const;
  void save (FILE *) const;
  void load (FILE *);

  std::map<jump_from, jump_to> jumps;
  int surrounding;

  std::map<int, context> expansion_contexts;
};

struct jump_src
{
  jump_src (int include, const jump_from& from)
    : include (include), from (from)
  {
  }

  jump_src (const jump_src& src)
    : include (src.include), from (src.from)
  {
  }

  int include;
  jump_from from;
};

struct jump_tgt
{
  jump_tgt (const jump_to& to, bool weak)
    : to (to), weak (weak)
  {
  }

  jump_tgt (const jump_tgt& tgt)
    : to (tgt.to), weak (tgt.weak)
  {
  }

  jump_to to;
  bool weak;
};

struct unit
{
  unit (const unit& unit)
    : log (unit.log),
      input (unit.input), input_id (unit.input_id),
      contexts (unit.contexts),
      file_map (unit.file_map),
      include_map (unit.include_map),
      point_map (unit.point_map)
  {
  }

  unit ()
    : log (NULL), input_id (0)
  {
  }

  unit (logger *log)
    : log (log), input_id (0)
  {
  }

  unit (logger *log, const std::string& input)
    : log (log),
      input (input), input_id (0)
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
    assert (point != 0);
    return get (include) ? get (include)->expansion (point) : NULL;
  }

  context *
  get (int include, int point)
  {
    assert (point != 0);
    return get (include)->expansion (include, point);
  }

  int
  get_expansion ()
  {
    int id = expansions.size () + 1;
    expansions.insert (std::make_pair (id, expansion ()));
    return id;
  }

  const expansion *
  get_expansion (int id) const
  {
    assert (id <= (int) expansions.size ());
    return &expansions.find (id)->second;
  }

  expansion *
  get_expansion (int id)
  {
    return (expansion *) ((const unit *) this)->get_expansion (id);
  }

  int file_id (const char *file);

  int
  include_id (const source_stack& include)
  {
    int id = include_map.get (include);
    if (input_id == 0
	&& include.locs.size () == 1
	&& include.locs.front ().fid == file_id (input.c_str ()))
      input_id = id;
    return id;
  }

  int
  point_id (const expansion_point& point)
  {
    return point_map.get (point);
  }

  void dump (FILE *, int) const;
  void save (FILE *) const;
  void load (FILE *);

  void
  trace (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log->vtrace (fmt, ap);
    va_end (ap);
  }

  void
  warning (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log->vwarning (fmt, ap);
    va_end (ap);
  }

  logger *log;
  std::string input;
  int input_id;
  std::map<int, context> contexts;
  std::map<int, expansion> expansions;
  id_map<std::string> file_map;
  id_map<source_stack> include_map;
  id_map<expansion_point> point_map;

  std::map<std::string, std::vector<jump_src> > pub_srcs;
  std::map<std::string, jump_tgt> pub_tgts;
};

enum set_flag
{
  SF_TRACE = 1,
  SF_DUMP = 2
};

struct set_data
{
  void save (const std::string& db) const;
  void load (const std::string& db);

  id_map<std::string> unit_map;
  id_map<std::string> ld_map;
  // ld_id => unit_id set
  std::map<int, std::set<int> > ld_units;
};

struct set
{
  set (const char *db, int flags)
    : log (stderr, flags & SF_TRACE),
      db (db), dump (flags & SF_DUMP), cur_id (0), cur_data (NULL)
  {
    trace ("load from %s\n", db);
    data.load (db);
  }

  ~set ()
  {
    if (cur_id)
      save_current_unit ();

    trace ("save to %s\n", db.c_str ());
    data.save (db);
  }

  void next (const std::string& args, const std::string& input);

  unit *
  current ()
  {
    return &cur;
  }

  const unit *
  current () const
  {
    return &cur;
  }

  int
  current_id () const
  {
    return cur_id;
  }

  void save_current_unit ();

  void
  trace (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log.vtrace (fmt, ap);
    va_end (ap);
  }

  void
  warning (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    log.vwarning (fmt, ap);
    va_end (ap);
  }

  logger log;
  std::string db;
  bool dump;

  set_data data;

  int cur_id;
  unit cur;

  void *cur_data;
};

struct set_usr
{
  set_usr (const std::string& db)
    : db(db)
  {
    data.load (db);
  }

  int get_ld (const char *name, const std::set<int>& units);
  const unit *get (int id);
  const unit *get (int ld_id, int id);

  std::string db;
  set_data data;
  std::map<int, unit> units;
  // ld_id => unit_id set
  std::map<int, std::map<int, unit> > ld_units;
};

struct unwind_stack
{
  macro_stack macro;
  source_stack include;
};

}
