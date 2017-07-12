#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <vector>

#include "elf.hpp"

#define SHN_UNDEF	0		/* Undefined section reference */

#define EI_CLASS	4	/* File class */
#define ELFCLASSNONE	      0	/* Invalid class */
#define ELFCLASS32	      1	/* 32-bit objects */
#define ELFCLASS64	      2	/* 64-bit objects */

#define EI_DATA		5	/* Data encoding */
#define ELFDATANONE	      0	/* Invalid data encoding */
#define ELFDATA2LSB	      1	/* 2's complement, little endian */
#define ELFDATA2MSB	      2	/* 2's complement, big endian */

#define EI_NIDENT	16		/* Size of e_ident[] */

typedef unsigned long bfd_vma;
typedef long file_ptr;
typedef unsigned long bfd_size_type;
typedef unsigned long long elf_vma;

struct elf_internal_ehdr {
  unsigned char		e_ident[EI_NIDENT]; /* ELF "magic number" */
  bfd_size_type		e_shoff;	/* Section header table file offset */
  unsigned int		e_shentsize;	/* Section header table entry size */
  unsigned int		e_shnum;	/* Section header table entry count */
  unsigned int		e_shstrndx;	/* Section header string table index */
};

struct elf_internal_shdr {
  unsigned int	sh_name;		/* Section name, index in string tbl */
  file_ptr	sh_offset;		/* Section file offset */
  bfd_size_type	sh_size;		/* Size of section in bytes */
};

struct elf_file
{
  elf_file ()
    : fp (NULL), byte_get (NULL)
  {
  }

  FILE *fp;

  elf_vma (*byte_get) (unsigned char *, int);

  elf_internal_ehdr ehdr;
  std::vector<elf_internal_shdr> shdrs;

  std::string strtab;
};

#define GET_BYTE(file, v) ((file)->byte_get ((v), sizeof (v)))

#define ARMAG  "!<arch>\012"
#define ARMAGT "!<thin>\012"
#define SARMAG 8

namespace elf
{

elf_vma
byte_get_little_endian (unsigned char *field, int size)
{
  switch (size)
    {
    case 1:
      return *field;

    case 2:
      return  ((unsigned int) (field[0]))
	|    (((unsigned int) (field[1])) << 8);

    case 3:
      return  ((unsigned long) (field[0]))
	|    (((unsigned long) (field[1])) << 8)
	|    (((unsigned long) (field[2])) << 16);

    case 4:
      return  ((unsigned long) (field[0]))
	|    (((unsigned long) (field[1])) << 8)
	|    (((unsigned long) (field[2])) << 16)
	|    (((unsigned long) (field[3])) << 24);

    case 5:
      if (sizeof (elf_vma) == 8)
	return  ((elf_vma) (field[0]))
	  |    (((elf_vma) (field[1])) << 8)
	  |    (((elf_vma) (field[2])) << 16)
	  |    (((elf_vma) (field[3])) << 24)
	  |    (((elf_vma) (field[4])) << 32);
      else if (sizeof (elf_vma) == 4)
	/* We want to extract data from an 8 byte wide field and
	   place it into a 4 byte wide field.  Since this is a little
	   endian source we can just use the 4 byte extraction code.  */
	return  ((unsigned long) (field[0]))
	  |    (((unsigned long) (field[1])) << 8)
	  |    (((unsigned long) (field[2])) << 16)
	  |    (((unsigned long) (field[3])) << 24);
      /* Fall through.  */

    case 6:
      if (sizeof (elf_vma) == 8)
	return  ((elf_vma) (field[0]))
	  |    (((elf_vma) (field[1])) << 8)
	  |    (((elf_vma) (field[2])) << 16)
	  |    (((elf_vma) (field[3])) << 24)
	  |    (((elf_vma) (field[4])) << 32)
	  |    (((elf_vma) (field[5])) << 40);
      else if (sizeof (elf_vma) == 4)
	/* We want to extract data from an 8 byte wide field and
	   place it into a 4 byte wide field.  Since this is a little
	   endian source we can just use the 4 byte extraction code.  */
	return  ((unsigned long) (field[0]))
	  |    (((unsigned long) (field[1])) << 8)
	  |    (((unsigned long) (field[2])) << 16)
	  |    (((unsigned long) (field[3])) << 24);
      /* Fall through.  */

    case 7:
      if (sizeof (elf_vma) == 8)
	return  ((elf_vma) (field[0]))
	  |    (((elf_vma) (field[1])) << 8)
	  |    (((elf_vma) (field[2])) << 16)
	  |    (((elf_vma) (field[3])) << 24)
	  |    (((elf_vma) (field[4])) << 32)
	  |    (((elf_vma) (field[5])) << 40)
	  |    (((elf_vma) (field[6])) << 48);
      else if (sizeof (elf_vma) == 4)
	/* We want to extract data from an 8 byte wide field and
	   place it into a 4 byte wide field.  Since this is a little
	   endian source we can just use the 4 byte extraction code.  */
	return  ((unsigned long) (field[0]))
	  |    (((unsigned long) (field[1])) << 8)
	  |    (((unsigned long) (field[2])) << 16)
	  |    (((unsigned long) (field[3])) << 24);
      /* Fall through.  */

    case 8:
      if (sizeof (elf_vma) == 8)
	return  ((elf_vma) (field[0]))
	  |    (((elf_vma) (field[1])) << 8)
	  |    (((elf_vma) (field[2])) << 16)
	  |    (((elf_vma) (field[3])) << 24)
	  |    (((elf_vma) (field[4])) << 32)
	  |    (((elf_vma) (field[5])) << 40)
	  |    (((elf_vma) (field[6])) << 48)
	  |    (((elf_vma) (field[7])) << 56);
      else if (sizeof (elf_vma) == 4)
	/* We want to extract data from an 8 byte wide field and
	   place it into a 4 byte wide field.  Since this is a little
	   endian source we can just use the 4 byte extraction code.  */
	return  ((unsigned long) (field[0]))
	  |    (((unsigned long) (field[1])) << 8)
	  |    (((unsigned long) (field[2])) << 16)
	  |    (((unsigned long) (field[3])) << 24);
      /* Fall through.  */

    default:
      assert (false);
    }
}

elf_vma
byte_get_big_endian (unsigned char *field, int size)
{
  switch (size)
    {
    case 1:
      return *field;

    case 2:
      return ((unsigned int) (field[1])) | (((int) (field[0])) << 8);

    case 3:
      return ((unsigned long) (field[2]))
	|   (((unsigned long) (field[1])) << 8)
	|   (((unsigned long) (field[0])) << 16);

    case 4:
      return ((unsigned long) (field[3]))
	|   (((unsigned long) (field[2])) << 8)
	|   (((unsigned long) (field[1])) << 16)
	|   (((unsigned long) (field[0])) << 24);

    case 5:
      if (sizeof (elf_vma) == 8)
	return ((elf_vma) (field[4]))
	  |   (((elf_vma) (field[3])) << 8)
	  |   (((elf_vma) (field[2])) << 16)
	  |   (((elf_vma) (field[1])) << 24)
	  |   (((elf_vma) (field[0])) << 32);
      else if (sizeof (elf_vma) == 4)
	{
	  /* Although we are extracting data from an 8 byte wide field,
	     we are returning only 4 bytes of data.  */
	  field += 1;
	  return ((unsigned long) (field[3]))
	    |   (((unsigned long) (field[2])) << 8)
	    |   (((unsigned long) (field[1])) << 16)
	    |   (((unsigned long) (field[0])) << 24);
	}
      /* Fall through.  */

    case 6:
      if (sizeof (elf_vma) == 8)
	return ((elf_vma) (field[5]))
	  |   (((elf_vma) (field[4])) << 8)
	  |   (((elf_vma) (field[3])) << 16)
	  |   (((elf_vma) (field[2])) << 24)
	  |   (((elf_vma) (field[1])) << 32)
	  |   (((elf_vma) (field[0])) << 40);
      else if (sizeof (elf_vma) == 4)
	{
	  /* Although we are extracting data from an 8 byte wide field,
	     we are returning only 4 bytes of data.  */
	  field += 2;
	  return ((unsigned long) (field[3]))
	    |   (((unsigned long) (field[2])) << 8)
	    |   (((unsigned long) (field[1])) << 16)
	    |   (((unsigned long) (field[0])) << 24);
	}
      /* Fall through.  */

    case 7:
      if (sizeof (elf_vma) == 8)
	return ((elf_vma) (field[6]))
	  |   (((elf_vma) (field[5])) << 8)
	  |   (((elf_vma) (field[4])) << 16)
	  |   (((elf_vma) (field[3])) << 24)
	  |   (((elf_vma) (field[2])) << 32)
	  |   (((elf_vma) (field[1])) << 40)
	  |   (((elf_vma) (field[0])) << 48);
      else if (sizeof (elf_vma) == 4)
	{
	  /* Although we are extracting data from an 8 byte wide field,
	     we are returning only 4 bytes of data.  */
	  field += 3;
	  return ((unsigned long) (field[3]))
	    |   (((unsigned long) (field[2])) << 8)
	    |   (((unsigned long) (field[1])) << 16)
	    |   (((unsigned long) (field[0])) << 24);
	}
      /* Fall through.  */

    case 8:
      if (sizeof (elf_vma) == 8)
	return ((elf_vma) (field[7]))
	  |   (((elf_vma) (field[6])) << 8)
	  |   (((elf_vma) (field[5])) << 16)
	  |   (((elf_vma) (field[4])) << 24)
	  |   (((elf_vma) (field[3])) << 32)
	  |   (((elf_vma) (field[2])) << 40)
	  |   (((elf_vma) (field[1])) << 48)
	  |   (((elf_vma) (field[0])) << 56);
      else if (sizeof (elf_vma) == 4)
	{
	  /* Although we are extracting data from an 8 byte wide field,
	     we are returning only 4 bytes of data.  */
	  field += 4;
	  return ((unsigned long) (field[3]))
	    |   (((unsigned long) (field[2])) << 8)
	    |   (((unsigned long) (field[1])) << 16)
	    |   (((unsigned long) (field[0])) << 24);
	}
      /* Fall through.  */

    default:
      assert (false);
    }
}

static std::string
process_archive (const char * /* name */,
		 elf_file * /* file */, bool /* thin */)
{
  return "not implemented";
}

struct elf32_external_shdr {
  unsigned char	sh_name[4];		/* Section name, index in string tbl */
  unsigned char	sh_type[4];		/* Type of section */
  unsigned char	sh_flags[4];		/* Miscellaneous section attributes */
  unsigned char	sh_addr[4];		/* Section virtual addr at execution */
  unsigned char	sh_offset[4];		/* Section file offset */
  unsigned char	sh_size[4];		/* Size of section in bytes */
  unsigned char	sh_link[4];		/* Index of another section */
  unsigned char	sh_info[4];		/* Additional section information */
  unsigned char	sh_addralign[4];	/* Section alignment */
  unsigned char	sh_entsize[4];		/* Entry size if section holds table */
};

struct elf64_external_shdr {
  unsigned char	sh_name[4];		/* Section name, index in string tbl */
  unsigned char	sh_type[4];		/* Type of section */
  unsigned char	sh_flags[8];		/* Miscellaneous section attributes */
  unsigned char	sh_addr[8];		/* Section virtual addr at execution */
  unsigned char	sh_offset[8];		/* Section file offset */
  unsigned char	sh_size[8];		/* Size of section in bytes */
  unsigned char	sh_link[4];		/* Index of another section */
  unsigned char	sh_info[4];		/* Additional section information */
  unsigned char	sh_addralign[8];	/* Section alignment */
  unsigned char	sh_entsize[8];		/* Entry size if section holds table */
};

static std::string
get_32bit_section_headers (elf_file *file)
{
  elf32_external_shdr *shdrs;
  if (file->ehdr.e_shentsize != sizeof *shdrs)
    return "invalid section entity size";

  unsigned int num = file->ehdr.e_shnum;
  unsigned int size = num * sizeof *shdrs;
  shdrs = (elf32_external_shdr *) malloc (size);
  if (pread (fileno (file->fp), shdrs, size, file->ehdr.e_shoff)
      != (ssize_t) size)
    {
      free (shdrs);
      return "error reading sections";
    }

  for (unsigned int i = 0; i < num; ++i)
    {
      elf_internal_shdr shdr;
      shdr.sh_name = GET_BYTE (file, shdrs[i].sh_name);
      shdr.sh_offset = GET_BYTE (file, shdrs[i].sh_offset);
      shdr.sh_size = GET_BYTE (file, shdrs[i].sh_size);
      file->shdrs.push_back (shdr);
    }
  free (shdrs);
  return std::string ();
}

static std::string
get_64bit_section_headers (elf_file *file)
{
  elf64_external_shdr *shdrs;
  if (file->ehdr.e_shentsize != sizeof *shdrs)
    return "invalid section entity size";

  unsigned int num = file->ehdr.e_shnum;
  unsigned int size = num * sizeof *shdrs;
  shdrs = (elf64_external_shdr *) malloc (size);
  if (pread (fileno (file->fp), shdrs, size, file->ehdr.e_shoff) != size)
    {
      free (shdrs);
      return "error reading sections";
    }

  for (unsigned int i = 0; i < num; ++i)
    {
      elf_internal_shdr shdr;
      shdr.sh_name = GET_BYTE (file, shdrs[i].sh_name);
      shdr.sh_offset = GET_BYTE (file, shdrs[i].sh_offset);
      shdr.sh_size = GET_BYTE (file, shdrs[i].sh_size);
      file->shdrs.push_back (shdr);
    }
  free (shdrs);
  return std::string ();
}

struct elf32_external_ehdr {
  unsigned char	e_ident[16];		/* ELF "magic number" */
  unsigned char	e_type[2];		/* Identifies object file type */
  unsigned char	e_machine[2];		/* Specifies required architecture */
  unsigned char	e_version[4];		/* Identifies object file version */
  unsigned char	e_entry[4];		/* Entry point virtual address */
  unsigned char	e_phoff[4];		/* Program header table file offset */
  unsigned char	e_shoff[4];		/* Section header table file offset */
  unsigned char	e_flags[4];		/* Processor-specific flags */
  unsigned char	e_ehsize[2];		/* ELF header size in bytes */
  unsigned char	e_phentsize[2];		/* Program header table entry size */
  unsigned char	e_phnum[2];		/* Program header table entry count */
  unsigned char	e_shentsize[2];		/* Section header table entry size */
  unsigned char	e_shnum[2];		/* Section header table entry count */
  unsigned char	e_shstrndx[2];		/* Section header string table index */
};

struct elf64_external_ehdr {
  unsigned char	e_ident[16];		/* ELF "magic number" */
  unsigned char	e_type[2];		/* Identifies object file type */
  unsigned char	e_machine[2];		/* Specifies required architecture */
  unsigned char	e_version[4];		/* Identifies object file version */
  unsigned char	e_entry[8];		/* Entry point virtual address */
  unsigned char	e_phoff[8];		/* Program header table file offset */
  unsigned char	e_shoff[8];		/* Section header table file offset */
  unsigned char	e_flags[4];		/* Processor-specific flags */
  unsigned char	e_ehsize[2];		/* ELF header size in bytes */
  unsigned char	e_phentsize[2];		/* Program header table entry size */
  unsigned char	e_phnum[2];		/* Program header table entry count */
  unsigned char	e_shentsize[2];		/* Section header table entry size */
  unsigned char	e_shnum[2];		/* Section header table entry count */
  unsigned char	e_shstrndx[2];		/* Section header string table index */
};

static std::string
process_object (elf_file *file)
{
  if (fread (file->ehdr.e_ident, EI_NIDENT, 1, file->fp) != 1)
    return "error reading elf file, failed in getting ident";

   switch (file->ehdr.e_ident[EI_DATA])
    {
    default:
    case ELFDATANONE:
    case ELFDATA2LSB:
      file->byte_get = byte_get_little_endian;
      break;
    case ELFDATA2MSB:
      file->byte_get = byte_get_big_endian;
      break;
    }

  /* For now we only support 32 bit and 64 bit ELF files.  */
  bool is_32bit_elf = (file->ehdr.e_ident[EI_CLASS] != ELFCLASS64);
 
  if (is_32bit_elf)
    {
      elf32_external_ehdr ehdr32;

      if (fread (ehdr32.e_type, sizeof ehdr32 - EI_NIDENT, 1, file->fp) != 1)
	return "error reading elf file, failed in getting header";

      file->ehdr.e_shoff     = GET_BYTE (file, ehdr32.e_shoff);
      file->ehdr.e_shentsize = GET_BYTE (file, ehdr32.e_shentsize);
      file->ehdr.e_shnum     = GET_BYTE (file, ehdr32.e_shnum);
      file->ehdr.e_shstrndx  = GET_BYTE (file, ehdr32.e_shstrndx);
    }
  else
    {
      elf64_external_ehdr ehdr64;

      assert (sizeof (bfd_vma) >= 8);
      if (fread (ehdr64.e_type, sizeof ehdr64 - EI_NIDENT, 1, file->fp) != 1)
	return "error reading elf file, failed in getting header";

      file->ehdr.e_shoff     = GET_BYTE (file, ehdr64.e_shoff);
      file->ehdr.e_shentsize = GET_BYTE (file, ehdr64.e_shentsize);
      file->ehdr.e_shnum     = GET_BYTE (file, ehdr64.e_shnum);
      file->ehdr.e_shstrndx  = GET_BYTE (file, ehdr64.e_shstrndx);
    }

  if (! file->ehdr.e_shoff)
    return "error reading elf file, no section found";

  if (file->ehdr.e_shnum == 0)
    return "no section found";

  if (file->ehdr.e_shstrndx == SHN_UNDEF
      || file->ehdr.e_shstrndx >= file->ehdr.e_shnum)
    return "invalid string table index";

  std::string err;
  if (is_32bit_elf)
    err = get_32bit_section_headers (file);
  else
    err = get_64bit_section_headers (file);

  if (! err.empty ())
    return err;

  file_ptr str_offset = file->shdrs[file->ehdr.e_shstrndx].sh_offset;
  bfd_size_type str_size = file->shdrs[file->ehdr.e_shstrndx].sh_size;
  char *strtab = (char *) malloc (str_size);
  if (pread (fileno (file->fp), strtab, str_size, str_offset)
      != (ssize_t) str_size)
    {
      free (strtab);
      return "error reading string table";
    }
  file->strtab = std::string (strtab, str_size);
  free (strtab);
  return std::string ();
}

static std::string
read_section (elf_file *file, const char *name, std::string *sec)
{
  std::vector<elf_internal_shdr>::iterator it;
  for (it = file->shdrs.begin (); it != file->shdrs.end (); ++it)
    if (it->sh_name >= file->strtab.size ())
      return "error getting section name";
    else if (strcmp (file->strtab.c_str () + it->sh_name, name) == 0)
      {
	char *buf = (char *) malloc (it->sh_size);
	bfd_size_type size = it->sh_size;
	if (pread (fileno (file->fp), buf, size, it->sh_offset)
	    != (ssize_t) size)
	  {
	    free (buf);
	    return "error reading section";
	  }
	*sec = std::string (buf, it->sh_size);
	free (buf);
	return std::string ();
      }
  return "section not found";
}

static int
get_int (const elf_file *file, const char *buf, int len)
{
  return file->byte_get ((unsigned char *) buf, len);
}

}

std::string
elf_reader::open (const char *name)
{
  file = new elf_file;

  file->fp = fopen (name, "rb");
  if (! file->fp)
    return "error opening elf file";

  char armag[SARMAG];
  if (fread (armag, SARMAG, 1, file->fp) == 1)
    {
      if (memcmp (armag, ARMAG, SARMAG) == 0)
	return elf::process_archive (name, file, false);
      else if (memcmp (armag, ARMAGT, SARMAG) == 0)
	return elf::process_archive (name, file, true);
    }

  rewind (file->fp);
  return elf::process_object (file);
}

void
elf_reader::close ()
{
  if (file->fp)
    fclose (file->fp);
  delete file;
}

std::string
elf_reader::read_section (const char *name, std::string *sec)
{
  return elf::read_section (file, name, sec);
}

int
elf_reader::get_int (const char *buf, int len) const
{
  return elf::get_int (file, buf, len);
}

#ifdef TEST
static bool
is_ok (std::string err)
{
  if (! err.empty ())
    printf ("%s\n", err.c_str ());
  return err.empty ();
}

int
main (int argc, const char **argv)
{
  elf_reader elf;
  assert (argc == 3);
  assert (is_ok (elf.open (argv[1])));
  std::string sec;
  assert (is_ok (elf.read_section (argv[2], &sec)));

  for (unsigned int i = 0; i < sec.size (); ++i)
    printf ("%02x%c", (unsigned char) sec[i], i % 16 != 15 ? ' ' : '\n');
  printf ("\n");

  for (unsigned int i = 0; i < sec.size (); i += 4)
    printf ("%08x%c", (unsigned int) elf.get_int (sec.c_str () + i, 4),
	    i % 4 != 3 ? ' ' : '\n');
  printf ("\n");

  return 0;
}
#endif
