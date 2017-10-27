#include <string>

struct elf_file;

struct elf_reader
{
  std::string open (const char* name);
  void close ();

  elf_reader ()
    : file (NULL)
  {
  }

  std::string read_section (const char* name, std::string* sec);
  int get_int (const char* buf, int len) const;

  elf_file* file;
};
