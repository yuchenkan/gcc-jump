#include <stdio.h>
#include <assert.h>

#include <string>
#include <map>
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

/*
struct src_loc
{
  int line;
  int col;
};

struct src_range
{
  src_loc start;
  src_loc end;
};

struct location
{
  bool virt;
  union
  {
    src_range range;
    int vid;
  } u;
};

struct virt_token
{
  int virt_id;
  std::string spell;
};

struct jumptgt
{
  int ctx_id;
  src_loc loc;
  std::vector<virt_token> expanded_tokens;
};

struct ctx_desc
{
  std::string file;
  int line;
  int col;
};

struct jumpctx
{
  jumpctx ()
    : next_virt_id (0)
  { }

  int next_desc_id;
  int next_virt_id;

  std::string file;

  std::map<ctx_desc, int> ctx_desc_id;
  std::map<std::vector<int>, int> virt_id_map;
  std::map<location, jumptgt> jumps;

  int surronding_ctx_id;
};
*/

struct source_location
{
  int fid;
  int line;
  int col;
};

struct file_map
{
  int
  id (const char *file)
  {
    return 0; // TODO
  }
};

struct jumpunit
{
  file_map files;
};

struct jumpset
{
  void next (const char *args);
  jumpunit *current ();
};

struct source_location_stack
{
  void
  add (source_location loc)
  {
    // TODO
  }
};

struct unwind_stack
{
  source_location_stack *
  macro ()
  {
    return NULL; // TODO
  }

  source_location_stack *
  include ()
  {
  }
};

}

static void
cb_start_unit (void *, void *data)
{
  gcj::jumpset *set = (gcj::jumpset *) data;

  std::string args;
  for (int i = 1; i < save_decoded_options_count; ++i)
    {
      fprintf (stderr, "option: %d %s %s",
	       save_decoded_options[i].opt_index,
	       save_decoded_options[i].arg,
	       save_decoded_options[i].orig_option_with_args_text);
      for (int j = 0; j < save_decoded_options[i].canonical_option_num_elements; ++j)
	{
	  fprintf (stderr, "\n  %s",
		   save_decoded_options[i].canonical_option[j]);

	  if (!args.empty()) args += ' ';
	  args += escape(save_decoded_options[i].canonical_option[j], ' ');
	}
      fprintf (stderr, "\n");
    }

  set->next (args.c_str ());

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

/*
static tree
walk_tree_node (tree *node, int *, void *)
{
  fprintf (stderr, "walk_tree_node: %s\n",
	   TREE_CODE (*node) >= MAX_TREE_CODES
	   ? "<unknown>" : get_tree_code_name (TREE_CODE (*node)));
  if (EXPR_P (*node))
    fprintf (stderr, "expr tree_operand_length: %d location: %s,%d,%d\n",
	     TREE_OPERAND_LENGTH (*node),
	     LOCATION_FILE (EXPR_LOCATION (*node)),
	     LOCATION_LINE (EXPR_LOCATION (*node)),
	     LOCATION_COLUMN (EXPR_LOCATION (*node)));
  else if (DECL_P (*node))
    fprintf (stderr, "decl name: %s location %s, %d,%d\n",
	     DECL_NAME (*node) ? IDENTIFIER_POINTER (DECL_NAME (*node)) : "<anonymous>",
	     DECL_SOURCE_FILE (*node),
	     DECL_SOURCE_LINE (*node),
	     DECL_SOURCE_COLUMN (*node));
  return NULL_TREE;
}

static void
cb_pre_genericize (void *fndecl, void *)
{
  fprintf (stderr, "pre_genericize\n");
  walk_tree (& DECL_SAVED_TREE ((tree)fndecl), walk_tree_node, NULL, NULL);
}
*/

static void
unwind_macro (gcj::jumpunit *junit, source_location loc,
	      gcj::source_location_stack *stack, const char *prefix)
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

      gcj::source_location jloc;
      jloc.fid = junit->files.id (LOCATION_FILE (l));
      jloc.line = LOCATION_LIEN (l);
      jloc.col = LOCATION_COLUMN (l);

      stack->add (jloc);
    }
}

static void
unwind_include (gcj::jumpunit *junit, source_location loc,
		gcj::source_location_stack *stack, const char *prefix)
{
  assert (loc > BUILTINS_LOCATION);

  const line_map_ordinary *m;
  linemap_resolve_location (line_table, loc,
			    LRK_MACRO_DEFINITION_LOCATION, &m);

  while (! MAIN_FILE_P (m))
    {
      m = INCLUDED_FROM (line_table, m);
      fprintf (stderr, "  %s, included from %s:%d\n",
	       prefix,
	       LINEMAP_FILE (m), LAST_SOURCE_LINE (m));

      gcj::source_location jloc;
      jloc.fid = junit->files.id (LINEMAP_FILE (m));
      jloc.line = LAST_SOURCE_LINE (m);
      jloc.col = 0;

      stack->add (jloc);
    }
}

static void
unwind (gcj::jumpunit *junit, source_location loc,
	gcj::unwind_stack *stack, const char *prefix)
{
  unwind_macro (junit, loc, stack->macro (), prefix);
  unwind_include (junit, loc, stack->include (), prefix);
}

static void
cb_cpp_token (void *arg, void  *)
{
  fprintf (stderr, "cpp_token\n");
  unwind (((cpp_token_arg *) arg)->loc, "cpp_token");
}

static void
cb_external_ref (void *arg, void *)
{
  tree decl = ((external_ref_arg *) arg)->decl;
  source_location loc = ((external_ref_arg *) arg)->loc;
  fprintf (stderr, "build_external_ref %s declared at %s:%d,%d\n",
	   IDENTIFIER_POINTER (DECL_NAME (decl)),
	   DECL_SOURCE_FILE (decl),
	   DECL_SOURCE_LINE (decl),
	   DECL_SOURCE_COLUMN (decl));
  unwind (DECL_SOURCE_LOCATION (decl), "declare");

  fprintf (stderr, "refered by %s:%d,%d\n",
	   LOCATION_FILE (loc),
	   LOCATION_LINE (loc),
	   LOCATION_COLUMN (loc));
  unwind (loc, "refer");
}

static void
cb_expand_macro (void *arg, void *data)
{
  gcj::jumpset *set = (gcj::jumpset *) data;

  const cpp_token *token = ((expand_macro_arg *) arg)->token;
  source_location loc = ((expand_macro_arg *) arg)->loc;
  source_location macro_loc = ((expand_macro_arg *) arg)->macro_loc;

  fprintf (stderr, "enter_macro %s %s:%d,%d\n",
	   token->type == CPP_NAME
	   ? (const char *) NODE_NAME (token->val.node.spelling) : "<unknown>",
	   LOCATION_FILE (loc), LOCATION_LINE (loc),
	   LOCATION_COLUMN (loc));

  gcj::unwind_stack stack;
  unwind (set->current (), loc, &stack, "macro");

  fprintf (stderr,
	   "enter_macro_context, macro %s defined at %s:%d,%d\n",
	   token->type == CPP_NAME
	   ? (const char *) NODE_NAME (token->val.node.spelling) : "<unknown>",
	   LOCATION_FILE (macro_loc), LOCATION_LINE (macro_loc),
	   LOCATION_COLUMN (macro_loc));
  unwind (macro_loc, "define");
}

static void
cb_finish (void *, void *data)
{
  fprintf (stderr, "finish\n");
  delete (gcj::jumpset *) data;
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

  gcj::jumpset *jumps = new gcj::jumpset;

  fprintf (stderr, "hello plugin\n");
  register_callback (plugin_info->base_name,
		     PLUGIN_START_UNIT,
		     cb_start_unit, jumps);
/*
  register_callback (plugin_info->base_name,
		     PLUGIN_PRE_GENERICIZE,
		     cb_pre_genericize, NULL);
*/

  register_callback (plugin_info->base_name,
		     PLUGIN_CPP_TOKEN,
		     cb_cpp_token, jumps);

  register_callback (plugin_info->base_name,
		     PLUGIN_EXTERNAL_REF,
		     cb_external_ref, jumps);

  register_callback (plugin_info->base_name,
		     PLUGIN_EXPAND_MACRO,
		     cb_expand_macro, jumps);

  register_callback (plugin_info->base_name,
		     PLUGIN_EXPAND_MACRO,
		     cb_expand_macro, jumps);

  register_callback (plugin_info->base_name,
		     PLUGIN_FINISH,
		     cb_finish, jumps);

  return 0;
}
