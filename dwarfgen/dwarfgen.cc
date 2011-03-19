/*
  Copyright (C) 2010 David Anderson.

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it would be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write the Free Software Foundation, Inc., 51
  Franklin Street - Fifth Floor, Boston MA 02110-1301, USA.

*/
// dwarfgen.cc
//  
// Using some information source, create a tree of dwarf
// information (speaking of a DIE tree).
// Turn the die tree into dwarfdata using libdwarf producer
// and write the resulting data in an object file.
// It is a bit inconsistent in error handling just to
// demonstrate the various possibilities using the producer
// library.
//
//  dwarfgen [-t def|obj|txt] [-o outpath] path

//  where -t means what sort of input to read
//         def means predefined (no input is read, the output
//         is based on some canned setups built into dwarfgen).
//         'path' is ignored in this case. This is the default.
//         
//         obj means 'path' is required, it is the object file to
//             read (the dwarf sections are duplicated in the output file)
//         
//         txt means 'path' is required, path must contain plain text
//             (in a form rather like output by dwarfdump)
//             that defines the dwarf that is to be output.
//
//  where  -o means specify the pathname of the output object. If not
//         supplied testout.o is used as the default output path.

#include "config.h"
#include <unistd.h> 
#include <stdlib.h> // for exit
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <list>
#include <vector>
#include <string.h> // For memset etc
#include <sys/stat.h> //open
#include <fcntl.h> //open
#include "general.h"
#include "elf.h"
#include "gelf.h"
#include "strtabdata.h"
#include "dwarf.h"
#include "libdwarf.h"
#include "irepresentation.h"
#include "ireptodbg.h"
#include "createirepfrombinary.h"

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::vector;

static void write_object_file(Dwarf_P_Debug dbg, IRepresentation &irep);
static void write_text_section(Elf * elf);
static void write_generated_dbg(Dwarf_P_Debug dbg,Elf * elf,
    IRepresentation &irep);

static string outfile("testout.o");
static string infile;
static enum  WhichInputSource { OptNone, OptReadText,OptReadBin,OptPredefined} 
    whichinput(OptPredefined);


// This is a global so thet CallbackFunc can get to it
static IRepresentation Irep;

static Elf * elf = 0;
static Elf32_Ehdr * ehp = 0;
static strtabdata secstrtab;
class SectionFromDwarf {
public:
     std::string name_;
     Dwarf_Unsigned section_name_itself_;
     ElfSymIndex section_name_symidx_;
     int size_;
     Dwarf_Unsigned type_;
     Dwarf_Unsigned flags_;
     Dwarf_Unsigned link_;
     Dwarf_Unsigned info_;
private:
     ElfSectIndex elf_sect_index_;
     Dwarf_Unsigned lengthWrittenToElf_;
public:
     Dwarf_Unsigned getNextOffset() { return lengthWrittenToElf_; }
     void setNextOffset(Dwarf_Unsigned v) { lengthWrittenToElf_ = v; }


     unsigned getSectionNameSymidx() { 
         return section_name_symidx_.getSymIndex(); };
     SectionFromDwarf():section_name_itself_(0),
         section_name_symidx_(0),
         size_(0),type_(0),flags_(0),
         link_(0), info_(0), elf_sect_index_(0),
         lengthWrittenToElf_(0) {} ;
     ~SectionFromDwarf() {};
     void setSectIndex(ElfSectIndex v) { elf_sect_index_ = v;}
     ElfSectIndex getSectIndex() const { return elf_sect_index_;}
     SectionFromDwarf(const std::string&name,
         int size,Dwarf_Unsigned type,Dwarf_Unsigned flags,
         Dwarf_Unsigned link, Dwarf_Unsigned info):
         name_(name),
         size_(size),type_(type),flags_(flags),
         link_(link), info_(info), elf_sect_index_(0),
         lengthWrittenToElf_(0) {
             // Now create section name string section.
             section_name_itself_ = secstrtab.addString(name.c_str());
             ElfSymbols& es = Irep.getElfSymbols();
             // Now creat a symbol for the section name.
             // (which has its own string table)
             section_name_symidx_  = es.addElfSymbol(0,name);
         } ;
};

vector<SectionFromDwarf> dwsectab;

static ElfSectIndex create_dw_elf(SectionFromDwarf  &ds);

static SectionFromDwarf & FindMySection(const ElfSectIndex & elf_section_index)
{
    for(unsigned i =0; i < dwsectab.size(); ++i) {
        if(elf_section_index.getSectIndex() != 
           dwsectab[i].getSectIndex().getSectIndex()) {
            continue;
        }
        return dwsectab[i];
    }
    cerr << "Unable to find my dw sec data for elf section " <<
        elf_section_index.getSectIndex() << endl;
    exit(1);
}

static unsigned
createnamestr(unsigned strtabstroff)
{
    Elf_Scn * strscn =elf_newscn(elf);
    if(!strscn) {
        cerr << "Unable to elf_newscn() on " << outfile << endl;
        exit(1);
    }
    Elf_Data* shstr =elf_newdata(strscn);
    if(!shstr) {
        cerr << "Unable to elf_newdata() on " << outfile << endl;
        exit(1);
    }
    shstr->d_buf = secstrtab.exposedata();
    shstr->d_type =  ELF_T_BYTE;
    shstr->d_size = secstrtab.exposelen();
    shstr->d_off = 0;
    shstr->d_align = 1;
    shstr->d_version = EV_CURRENT;

    Elf32_Shdr * strshdr = elf32_getshdr(strscn);
    if(!strshdr) {
        cerr << "Unable to elf_getshdr() on " << outfile << endl;
        exit(1);
    }
    strshdr->sh_name =  strtabstroff; 
    strshdr->sh_type= SHT_STRTAB;
    strshdr->sh_flags = SHF_STRINGS;
    strshdr->sh_addr = 0;
    strshdr->sh_offset = 0;
    strshdr->sh_size = 0;
    strshdr->sh_link  = 0;
    strshdr->sh_info = 0;
    strshdr->sh_addralign = 1;
    strshdr->sh_entsize = 0;
    return  elf_ndxscn(strscn);
}


// This functional interface is defined by libdwarf. 
int CallbackFunc(
    char* name,
    int                 size,
    Dwarf_Unsigned      type,
    Dwarf_Unsigned      flags,
    Dwarf_Unsigned      link,
    Dwarf_Unsigned      info,
    Dwarf_Unsigned*     sect_name_symbol_index,
    int*                error) 
{
    // Create an elf section.
    // If the data is relocations, we suppress the generation
    // of a section when we intend to do the relocations
    // ourself (quite normal for dwarfgen but would
    // be really surprising for a normal compiler
    // back end using the producer code).

    // The section name appears both in the section strings .shstrtab and
    // in the elf symtab .symtab and its strings .strtab.

    if (0 == strncmp(name,".rel",4))  {
         // It is relocation, create no section!
         return 0;
    }
    SectionFromDwarf ds(name,size,type,flags,link,info) ;
    *sect_name_symbol_index = ds.getSectionNameSymidx();
    ElfSectIndex createdsec = create_dw_elf(ds);

    // Do all the data creation before pushing (copying) ds onto dwsectab!
    dwsectab.push_back(ds);
    return createdsec.getSectIndex();
}

static ElfSectIndex
create_dw_elf(SectionFromDwarf  &ds)
{
    Elf_Scn * scn =elf_newscn(elf);
    if(!scn) {
        cerr << "Unable to elf_newscn() on " << ds.name_  << endl;
        exit(1);
    }
    Elf32_Shdr * shdr = elf32_getshdr(scn);
    if(!shdr) {
        cerr << "Unable to elf_getshdr() on " << ds.name_ << endl;
        exit(1);
    }
    shdr->sh_name   = ds.section_name_itself_;
    shdr->sh_type   = ds.type_;
    shdr->sh_flags  = ds.flags_;
    shdr->sh_addr   = 0;
    shdr->sh_offset = 0;
    shdr->sh_size   = ds.size_;
    shdr->sh_link   = ds.link_;
    shdr->sh_info   = ds.info_;
    shdr->sh_addralign = 1;
    shdr->sh_entsize = 0;
    ElfSectIndex si(elf_ndxscn(scn));
    ds.setSectIndex(si);
    return  si;
}

// Default error handler of libdwarf producer code.
void ErrorHandler(Dwarf_Error err,Dwarf_Ptr errarg)
{
    // FIXME do better error handling
    cerr <<"Giving up, encountered an error" << endl;
    exit(1);
}


static void
setinput(enum  WhichInputSource *src,
   const string &type,
   bool *pathreq)
{
   if(type == "txt") {
      *src = OptReadText;
      *pathreq = true;
      return;
   } else if (type == "obj") {
      *src = OptReadBin;
      *pathreq = true;
      return;
   } else if (type == "def") {
      *src = OptPredefined;
      *pathreq = false;
      return;
   }
   cout << "Giving up, only txt obj or def accepted after -t" << endl;
   exit(1);
}

int 
main(int argc, char **argv)
{


    int opt;
    bool pathrequired(false);
    long cu_of_input_we_output = -1;
    while((opt=getopt(argc,argv,"o:t:c:")) != -1) {
        switch(opt) {
        case 'c':
            // At present we can only create a single
            // cu in the output of the libdwarf producer.
            cu_of_input_we_output = atoi(optarg);
            break;
        case 't':
            setinput(&whichinput,optarg,&pathrequired);
            break;
        case 'o':
            outfile = optarg;
            break;
        case '?':
            cerr << "Invalid quest? option input " << endl;
            exit(1);
            
        default:
            cerr << "Invalid option input " << endl;
            exit(1);
        }
    }
    if ( (optind >= argc) && pathrequired) {
        cerr << "Expected argument after options! Giving up." << endl;
        exit(EXIT_FAILURE);
    }

    if(pathrequired) {
        infile = argv[optind];    
    }
    
    if(whichinput == OptReadBin) {
        createIrepFromBinary(infile,Irep);
    } else if (whichinput == OptReadText) {
        cerr << "dadebug text read not supported yet" << endl;
        exit(EXIT_FAILURE);
    } else if (whichinput == OptPredefined) {
        cerr << "dadebug predefined not supported yet" << endl;
        exit(EXIT_FAILURE);
    } else {
        cerr << "Impossible: unknown input style." << endl;
        exit(EXIT_FAILURE);
    }
    
    // Example will return error value thru 'err' pointer 
    // and return DW_DLV_BADADDR if there is an error.
    int ptrsize = DW_DLC_SIZE_32;
    Dwarf_Ptr errarg = 0;
    Dwarf_Error err = 0;
    // We use DW_DLC_SYMBOLIC_RELOCATIONS so we can
    // read the relocations and do our own relocating.
    // See calls of dwarf_get_relocation_info().
    Dwarf_P_Debug dbg = dwarf_producer_init_b(
        DW_DLC_WRITE|ptrsize|DW_DLC_SYMBOLIC_RELOCATIONS,
        CallbackFunc,
        0,
        errarg,
        &err);
    if(dbg == reinterpret_cast<Dwarf_P_Debug>(DW_DLV_BADADDR)) {
        cerr << "Failed init_b" << endl;
        exit(EXIT_FAILURE);
    }
        
    transform_irep_to_dbg(dbg,Irep,cu_of_input_we_output);

    write_object_file(dbg,Irep);
    // Example calls ErrorHandler if there is an error
    // (which does not return, see above)
    // so no need to test for error.
    dwarf_producer_finish( dbg, 0);

    return 0;
}

static void 
write_object_file(Dwarf_P_Debug dbg, IRepresentation &irep)
{
    int mode =  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    int fd = open(outfile.c_str(),O_WRONLY|O_CREAT|O_TRUNC, mode);
    if(fd < 0 ) {
        cerr << "Unable to open " << outfile << 
            " for writing." << endl;
        exit(1);
    }

    if(elf_version(EV_CURRENT) == EV_NONE) {
        cerr << "Bad elf_version" << endl;
        exit(1);
    }

    Elf_Cmd cmd = ELF_C_WRITE;
    elf = elf_begin(fd,cmd,0);
    if(!elf) {
        cerr << "Unable to elf_begin() on " << outfile << endl;
        exit(1);
    }
    ehp = elf32_newehdr(elf);
    if(!ehp) {
        cerr << "Unable to elf_newehdr() on " << outfile << endl;
        exit(1);
    }
    ehp->e_ident[EI_MAG0] = ELFMAG0; 
    ehp->e_ident[EI_MAG1] = ELFMAG1; 
    ehp->e_ident[EI_MAG2] = ELFMAG2; 
    ehp->e_ident[EI_MAG3] = ELFMAG3; 
    ehp->e_ident[EI_CLASS] = ELFCLASS32; 
    ehp->e_ident[EI_DATA] = ELFDATA2LSB;
    ehp->e_ident[EI_VERSION] = EV_CURRENT;
    ehp->e_machine = EM_386;
    ehp->e_type = ET_EXEC;
    ehp->e_version = EV_CURRENT;

    unsigned  strtabstroff = secstrtab.addString(".shstrtab");

    // an object section with fake .text data (just as an example).
    write_text_section(elf);

    write_generated_dbg(dbg,elf,Irep);

    // Now create section name string section.
    unsigned shstrindex = createnamestr(strtabstroff);
    ehp->e_shstrndx = shstrindex;

    off_t ures = elf_update(elf,cmd);
    if(ures == (off_t)(-1LL)) {
        cerr << "Unable to elf_update() on " << outfile << endl;
        int eer = elf_errno();
        cerr << "Error is " << eer << " " << elf_errmsg(eer) << endl;
        exit(1);
    }
    cout << " output image size in bytes " << ures << endl;

    elf_end(elf);
    close(fd);
}

    
// an object section with fake .text data (just as an example).
static void
write_text_section(Elf * elf)
{
    unsigned  osecnameoff = secstrtab.addString(".text");
    Elf_Scn * scn1 =elf_newscn(elf);
    if(!scn1) {
        cerr << "Unable to elf_newscn() on " << outfile << endl;
        exit(1);
    }

    Elf_Data* ed1 =elf_newdata(scn1);
    if(!ed1) {
        cerr << "Unable to elf_newdata() on " << outfile << endl;
        exit(1);
    }
    const char *d = "data in section";
    ed1->d_buf = (void *)d;
    ed1->d_type =  ELF_T_BYTE;
    ed1->d_size = strlen(d) +1;
    ed1->d_off = 0;
    ed1->d_align = 4;
    ed1->d_version = EV_CURRENT;
    Elf32_Shdr * shdr1 = elf32_getshdr(scn1);
    if(!shdr1) {
        cerr << "Unable to elf_getshdr() on " << outfile << endl;
        exit(1);
    }
    shdr1->sh_name =  osecnameoff;
    shdr1->sh_type= SHT_PROGBITS;
    shdr1->sh_flags = 0;
    shdr1->sh_addr = 0;
    shdr1->sh_offset = 0;
    shdr1->sh_size = 0;
    shdr1->sh_link  = 0;
    shdr1->sh_info = 0;
    shdr1->sh_addralign = 1;
    shdr1->sh_entsize = 0;
}
static void 
InsertDataIntoElf(Dwarf_Signed d,Dwarf_P_Debug dbg,Elf *elf)
{
    Dwarf_Signed elf_section_index = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Ptr bytes = dwarf_get_section_bytes(dbg,d,
        &elf_section_index,&length,0);

    Elf_Scn *scn =  elf_getscn(elf,elf_section_index);
    if(!scn) {
        cerr << "Unable to elf_getscn on disk transform # " << d << endl;
        exit(1);
    }

    ElfSectIndex si(elf_section_index);
    SectionFromDwarf & sfd  = FindMySection(si);

    Elf_Data* ed =elf_newdata(scn);
    if(!ed) {
        cerr << "elf_newdata died on transformed index " << d << endl;
        exit(1);
    }
    ed->d_buf = bytes;
    ed->d_type =  ELF_T_BYTE;
    ed->d_size = length;
    ed->d_off = sfd.getNextOffset();
    sfd.setNextOffset(ed->d_off + length);
    ed->d_align = 1;
    ed->d_version = EV_CURRENT;
    cout << "Inserted " << length << " bytes into elf section index " << 
        elf_section_index << endl;
}

static string
printable_rel_type(unsigned char reltype)
{
     enum Dwarf_Rel_Type t = (enum Dwarf_Rel_Type)reltype;
     switch(t) {
     case   dwarf_drt_none:
          return "dwarf_drt_none";
     case   dwarf_drt_data_reloc:
          return "dwarf_drt_data_reloc";
     case   dwarf_drt_segment_rel:
          return "dwarf_drt_segment_rel";
     case   dwarf_drt_first_of_length_pair:
          return "dwarf_drt_first_of_length_pair";
     case   dwarf_drt_second_of_length_pair:
          return "dwarf_drt_second_of_length_pair";
     default:
          break;
     }
     return "drt-unknown (impossible case)";
}

static Dwarf_Unsigned  
FindSymbolValue(ElfSymIndex symi,IRepresentation &irep)
{
    ElfSymbols & syms = irep.getElfSymbols();
    ElfSymbol & es =  syms.getElfSymbol(symi);
    Dwarf_Unsigned symv = es.getSymbolValue();
    return symv;
}

static void
bitreplace(char *buf, Dwarf_Unsigned newval,
           size_t newvalsize,int length)
{
     uint64_t my8;
     if(length == 4) {
         uint32_t my4 = newval; 
         uint32_t * p = reinterpret_cast<uint32_t *>(buf );
         uint32_t oldval = *p;
         *p = oldval + my4;
//cout << "dadebug old val " << IToHex(oldval) << "  newval " << IToHex(*p )<< endl;
     } else if (length == 8) {
         uint64_t my8 = newval; 
         uint64_t * p = reinterpret_cast<uint64_t *>(buf );
         uint64_t oldval = *p;
         *p = oldval + my8;
//cout << "dadebug old val " << IToHex(oldval) << "  newval " << IToHex(*p) << endl;
     } else {
         cerr << " Relocation is length " << length << 
             " which we do not yet handle." << endl;
         exit(1);
     }
}

// This remembers nothing, so is dreadfully slow.
static char *
findelfbuf(Elf *elf,Elf_Scn *scn,Dwarf_Unsigned offset, unsigned length)
{
    Elf_Data * edbase = 0;
    Elf_Data * ed = elf_getdata(scn,edbase);
    unsigned bct = 0;
    for (;ed; ed = elf_getdata(scn,ed)) {
          bct++;
          if(offset >= (ed->d_off + ed->d_size) ) {
//cout << "dadebug ignore buf, count " << bct << " off " << ed->d_off << "  size " << ed->d_size << "  end  " << (ed->d_off +ed->d_size) << endl;
              continue;
          }
          if(offset < ed->d_off) {
              cerr << " Relocation at offset  " << 
               offset << " cannot be accomplished, no buffer. " 
               << endl; 
              exit(1);
          }
          Dwarf_Unsigned localoff = offset - ed->d_off;
          if((localoff + length) > ed->d_size) {
              cerr << " Relocation at offset  " << 
               offset << " cannot be accomplished, size mismatch. " 
               << endl; 
              exit(1);
          }
//cout << "dadebug found buf, count " << bct << " off " << ed->d_off << "  size " << ed->d_size << "  end  " << (ed->d_off +ed->d_size) << endl;
          char *lclptr = reinterpret_cast<char *>(ed->d_buf) + localoff;
          return lclptr;
         
    }
    cerr << " Relocation at offset  " << offset  << 
        " cannot be accomplished,  past end of buffers" << endl; 
    return 0;

}

static void
write_generated_dbg(Dwarf_P_Debug dbg,Elf * elf,IRepresentation &irep)
{
    Dwarf_Error err = 0;
    Dwarf_Signed sectioncount = 
        dwarf_transform_to_disk_form(dbg,0);

    Dwarf_Signed d = 0;
    for(d = 0; d < sectioncount ; ++d) {
        InsertDataIntoElf(d,dbg,elf);
    }

    // Since we are emitting in final form sometimes, we may
    // do relocation processing here or we may
    // instead emit relocation records into the object file.
    // The following is for DW_DLC_SYMBOLIC_RELOCATIONS.
    Dwarf_Unsigned reloc_sections_count = 0;
    int drd_version = 0;
    int res = dwarf_get_relocation_info_count(dbg,&reloc_sections_count,
        &drd_version,&err);
    if( res != DW_DLV_OK) {
        cerr << "Error getting relocation info count." << endl;
        exit(1);

    }
    cout << "Relocations sections count= " << reloc_sections_count << 
        " relversion=" << drd_version << endl;
    for( Dwarf_Unsigned ct = 0; ct < reloc_sections_count ; ++ct) {
          // elf_section_index is the elf index of the relocations
          // themselves. 
          Dwarf_Signed elf_section_index = 0;
          // elf_section_index_link is the elf index of the section
          // the relocations apply to.
          Dwarf_Signed elf_section_index_link = 0;
          // relocation_buffer_count is the number of relocations
          // of this section.
          Dwarf_Unsigned relocation_buffer_count = 0;
          Dwarf_Relocation_Data reld;
          res = dwarf_get_relocation_info(dbg,&elf_section_index,
              &elf_section_index_link,
              &relocation_buffer_count,
              &reld,&err);
          if (res != DW_DLV_OK) {
             cerr << "Error getting relocation record " <<
                  ct << "."  << endl;
             exit(1);
          }
          ElfSectIndex si(elf_section_index_link);
          SectionFromDwarf & sfd  = FindMySection(si);
          cout << "Relocs for sec " << ct << " elf-sec=" << elf_section_index <<
              " link="      << elf_section_index_link <<
              " bufct="     << relocation_buffer_count << endl;
          Elf_Scn *scn =  elf_getscn(elf,si.getSectIndex());
          if(!scn) {
              cerr << "Unable to elf_getscn  # " << si.getSectIndex() << endl;
              exit(1);
          }

          for (Dwarf_Unsigned r = 0; r < relocation_buffer_count; ++r) {
              Dwarf_Relocation_Data rec = reld+r;
//cout << " dadebug " << " type= " << static_cast<int>(rec->drd_type) <<
 //                 "  len= " << static_cast<int>(rec->drd_length) <<
  //                "  offset= " << rec->drd_offset <<
   //               "  symindex= " << rec->drd_symbol_index << endl;
              ElfSymIndex symi(rec->drd_symbol_index);
              Dwarf_Unsigned newval = FindSymbolValue(symi,irep);
              char *buf_to_update = findelfbuf(elf,scn,
                  rec->drd_offset,rec->drd_length);
              if(buf_to_update) {
                  bitreplace(buf_to_update, newval,sizeof(newval),
                  rec->drd_length);
              }
          }
    }

}
