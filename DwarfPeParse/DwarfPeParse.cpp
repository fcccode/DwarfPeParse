// DwarfPeParse.cpp : Defines the entry point for the console application.
//

#include "pch.h"

typedef struct
{
	HANDLE hFileMapping;
	DWORD_PTR szFileSize;
	union {
		PBYTE lpFileBase;
		PIMAGE_DOS_HEADER pDosHeader;
	};

	PIMAGE_NT_HEADERS pNtHeaders;
	PIMAGE_SECTION_HEADER Sections;
	PIMAGE_SYMBOL pSymbolTable;
	PSTR pStringTable;
}PeObject;

static int pe_get_section_info(void *obj, Dwarf_Half section_index,
	Dwarf_Obj_Access_Section *return_section,
	int *error)
{
	PeObject* pe_obj = (PeObject*)obj;
	return_section->addr = 0;
	if (section_index == 0)
	{
		return_section->size = 0;
		return_section->name = "";
	}
	else
	{
		PIMAGE_SECTION_HEADER pSection = pe_obj->Sections + section_index - 1;
		if (pSection->Misc.VirtualSize < pSection->SizeOfRawData)
			return_section->size = pSection->Misc.VirtualSize;
		else
			return_section->size = pSection->SizeOfRawData;

		return_section->name = (const char*)pSection->Name;
		if (return_section->name[0] == '/') {
			return_section->name = &pe_obj->pStringTable[atoi(&return_section->name[1])];
		}
	}
	return_section->link = 0;
	return_section->entrysize = 0;

	return DW_DLV_OK;
}

static Dwarf_Endianness pe_get_byte_order(void *obj)
{
	return DW_OBJECT_LSB;
}

static Dwarf_Small pe_get_length_pointer_size(void *obj)
{
	PeObject *pe_obj = (PeObject *)obj;
	PIMAGE_OPTIONAL_HEADER pOptionalHeader = &pe_obj->pNtHeaders->OptionalHeader;

	switch (pOptionalHeader->Magic)
	{
	case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
		return 4;
	case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
		return 8;
	default:
		return 0;
	}
}

static Dwarf_Unsigned pe_get_section_count(void *obj)
{
	PeObject *pe_obj = (PeObject*)obj;
	PIMAGE_FILE_HEADER pFileHeader = &pe_obj->pNtHeaders->FileHeader;
	return pFileHeader->NumberOfSections + 1;
}

static int pe_load_section(void *obj,
	Dwarf_Half section_index,
	Dwarf_Small **return_data,
	int *error)
{
	PeObject* pe_obj = (PeObject*)obj;
	if (section_index == 0)
		return DW_DLV_NO_ENTRY;
	else {
		PIMAGE_SECTION_HEADER pSection = pe_obj->Sections + section_index - 1;
		*return_data = pe_obj->lpFileBase + pSection->PointerToRawData;
		return DW_DLV_OK;
	}
}

// define the methods for the dwarflib object interface
static const Dwarf_Obj_Access_Methods pe_methods
{
	pe_get_section_info,
	pe_get_byte_order,
	pe_get_length_pointer_size,
	pe_get_length_pointer_size,
	pe_get_section_count,
	pe_load_section
};


int dwarf_pe_init(HANDLE hFile, const TCHAR* chFilePath,
	Dwarf_Handler errHandler, Dwarf_Ptr errArg,
	Dwarf_Debug *retDbg, Dwarf_Error *dwErr)
{
	int res = DW_DLV_ERROR;
	PeObject* pe_obj;
	LARGE_INTEGER lpSize;

	pe_obj = (PeObject*)calloc(1, sizeof(PeObject));
	if (!pe_obj)
		return res;

	pe_obj->hFileMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!pe_obj) {
		goto failmap;
	}

	//Map view
	pe_obj->lpFileBase = (PBYTE)MapViewOfFile(pe_obj->hFileMapping, FILE_MAP_READ, 0, 0, 0);
	if (!pe_obj->lpFileBase) {
		goto failmapview;
	}

	if (!GetFileSizeEx(hFile, &lpSize)) {
		goto badsize;
	}

#ifdef _WIN64
	pe_obj->szFileSize = lpSize.QuadPart;
#else
	pe_obj->szFileSize = lpSize.LowPart;
#endif

	// fill in PE headers
	pe_obj->pNtHeaders = (PIMAGE_NT_HEADERS)(
		pe_obj->lpFileBase + pe_obj->pDosHeader->e_lfanew);

	// Pointer to PE sections table
	pe_obj->Sections = IMAGE_FIRST_SECTION(pe_obj->pNtHeaders);

	// Symbol Table
	pe_obj->pSymbolTable = (PIMAGE_SYMBOL)(
		pe_obj->lpFileBase + pe_obj->pNtHeaders->FileHeader.PointerToSymbolTable);

	// String table starts at end of symbol table
	pe_obj->pStringTable = (PSTR)&pe_obj->pSymbolTable[pe_obj->pNtHeaders->FileHeader.NumberOfSymbols];

	// TODO: support external symbol files (drmingw.dwarf_pe.cpp:208 for ref) 

	// Create the Dwarf Object Access Interface
	Dwarf_Obj_Access_Interface* dwarfInterface;

	dwarfInterface = (Dwarf_Obj_Access_Interface*)calloc(1, sizeof(Dwarf_Obj_Access_Interface));
	if (!dwarfInterface)
		goto nointerface;

	//assign the object
	dwarfInterface->object = pe_obj;
	dwarfInterface->methods = &pe_methods;

	res = dwarf_object_init(dwarfInterface, errHandler, errArg, retDbg, dwErr);
	if (res == DW_DLV_OK) {
		return res;
	}

badsize:
nointerface:
	UnmapViewOfFile(pe_obj->lpFileBase);
failmapview:
	CloseHandle(pe_obj->hFileMapping);
failmap:
	free(pe_obj);

	return res;
}

int dwarf_pe_finish(Dwarf_Debug dbg, Dwarf_Error *err)
{
	Dwarf_Obj_Access_Interface *intf = dbg->de_obj_file;
	PeObject *pe_obj = (PeObject*)intf->object;
	free(intf);
	UnmapViewOfFile(pe_obj->lpFileBase);
	CloseHandle(pe_obj->hFileMapping);
	free(pe_obj);
	return dwarf_object_finish(dbg, err);
}


static void print_die_data(Dwarf_Debug dbg, Dwarf_Die print_me, int level)
{
	char *name = 0;
	Dwarf_Error error = 0;
	Dwarf_Half tag = 0;
	const char *tagname = 0;
	int res = dwarf_diename(print_me, &name, &error);
	if (res == DW_DLV_ERROR) {
		printf("Error in dwarf_diename , level %d \n", level);
		exit(1);
	}
	if (res == DW_DLV_NO_ENTRY) {
		return;
	}
	res = dwarf_tag(print_me, &tag, &error);
	if (res != DW_DLV_OK) {
		printf("Error in dwarf_tag , level %d \n", level);
		exit(1);
	}
	res = dwarf_get_TAG_name(tag, &tagname);
	if (res != DW_DLV_OK) {
		printf("Error in dwarf_get_TAG_name , level %d \n", level);
		exit(1);
	}
	printf("<%d> tag: %d %s  name: %s\n", level, tag, tagname, name);
	dwarf_dealloc(dbg, name, DW_DLA_STRING);
}

static void get_die_and_siblings(Dwarf_Debug dbg, Dwarf_Die in_die, int in_level)
{
	int res = DW_DLV_ERROR;
	Dwarf_Die cur_die = in_die;
	Dwarf_Die child = 0;
	Dwarf_Error error;
	print_die_data(dbg, in_die, in_level);

	for (;;) {
		Dwarf_Die sib_die = 0;
		res = dwarf_child(cur_die, &child, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_child , level %d \n", in_level);
			exit(1);
		}
		if (res == DW_DLV_OK) {
			get_die_and_siblings(dbg, child, in_level + 1);
		}
		/* res == DW_DLV_NO_ENTRY */
		res = dwarf_siblingof(dbg, cur_die, &sib_die, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_siblingof , level %d \n", in_level);
			exit(1);
		}
		if (res == DW_DLV_NO_ENTRY) {
			/* Done at this level. */
			break;
		}
		/* res == DW_DLV_OK */
		if (cur_die != in_die) {
			dwarf_dealloc(dbg, cur_die, DW_DLA_DIE);
		}
		cur_die = sib_die;
	}
	return;
}

static void read_cu_list(Dwarf_Debug dbg)
{
	Dwarf_Unsigned cu_header_length = 0;
	Dwarf_Half version_stamp = 0;
	Dwarf_Unsigned abbrev_offset = 0;
	Dwarf_Half address_size = 0;
	Dwarf_Unsigned next_cu_header = 0;
	Dwarf_Error error;
	int cu_number = 0;

	for (;; ++cu_number) {
		Dwarf_Die no_die = 0;
		Dwarf_Die cu_die = 0;
		int res = DW_DLV_ERROR;
		res = dwarf_next_cu_header(dbg, &cu_header_length,
			&version_stamp, &abbrev_offset, &address_size,
			&next_cu_header, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_next_cu_header\n");
			exit(1);
		}
		if (res == DW_DLV_NO_ENTRY) {
			/* Done. */
			return;
		}
		/* The CU will have a single sibling, a cu_die. */
		res = dwarf_siblingof(dbg, no_die, &cu_die, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_siblingof on CU die \n");
			exit(1);
		}
		if (res == DW_DLV_NO_ENTRY) {
			/* Impossible case. */
			printf("no entry! in dwarf_siblingof on CU die \n");
			exit(1);
		}
		get_die_and_siblings(dbg, cu_die, 0);
		dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
	}
}

int main(int argc, char** argv)
{
	Dwarf_Debug dbg = 0;
	//int fd = -1;
	FILE* fd = nullptr;


	//choose any executable with dwarf debug symbols
	const TCHAR* filepath = TEXT("DwarfGenName.exe");
	int res = DW_DLV_ERROR;
	Dwarf_Error err;
	Dwarf_Handler errhandler = 0;
	Dwarf_Ptr errarg = 0;
	HANDLE hFile = NULL;

	// open the file
	hFile = CreateFile(filepath, GENERIC_READ,
		FILE_SHARE_READ, NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		0);

	res = dwarf_pe_init(hFile, filepath, errhandler, errarg, &dbg, &err);
	if (res != DW_DLV_OK) {
		printf("Could not process DWARF symbols\n");
		exit(1);
	}

	read_cu_list(dbg);
	res = dwarf_pe_finish(dbg, &err);
	if (res != DW_DLV_OK) {
		printf("Dwarf finish failed\n");
	}

	CloseHandle(hFile);
	getchar();
	return 0;
}

