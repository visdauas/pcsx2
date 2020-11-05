/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "Utilities/SafeArray.inl"
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/stopwatch.h>

#include <chrono>

// IMPORTANT!  If this gets a macro redefinition error it means PluginCallbacks.h is included
// in a global-scope header, and that's a BAD THING.  Include it only into modules that need
// it, because some need to be able to alter its behavior using defines.  Like this:

struct Component_FileMcd;
#define PS2E_THISPTR Component_FileMcd*

#include "MemoryCardFile.h"
#include "MemoryCardFolder.h"

#include "System.h"
#include "AppConfig.h"

#include "svnrev.h"

#include "ConsoleLogger.h"

#include <wx/ffile.h>
#include <map>

static const int MCD_SIZE	= 1024 *  8  * 16;		// Legacy PSX card default size

static const int MC2_MBSIZE	= 1024 * 528 * 2;		// Size of a single megabyte of card data

// --------------------------------------------------------------------------------------
//  FileMemoryCard
// --------------------------------------------------------------------------------------
// Provides thread-safe direct file IO mapping.
//
class FileMemoryCard
{
protected:
	wxFFile			m_file[8];
	u8				m_effeffs[528*16];
	SafeArray<u8>	m_currentdata;
	u64				m_chksum[8];
	bool			m_ispsx[8];
	u32				m_chkaddr;

public:
	FileMemoryCard();
	virtual ~FileMemoryCard() = default;

	void Lock();
	void Unlock();

	void Open();
	void Close();

	s32  IsPresent	( uint slot );
	void GetSizeInfo( uint slot, PS2E_McdSizeInfo& outways );
	bool IsPSX	( uint slot );
	s32  Read		( uint slot, u8 *dest, u32 adr, int size );
	s32  Save		( uint slot, const u8 *src, u32 adr, int size );
	s32  EraseBlock	( uint slot, u32 adr );
	u64  GetCRC		( uint slot );

protected:
	bool Seek( wxFFile& f, u32 adr );
	bool Create( const wxString& mcdFile, uint sizeInMB );

	wxString GetDisabledMessage( uint slot ) const
	{
		return wxsFormat( pxE( L"The PS2-slot %d has been automatically disabled.  You can correct the problem\nand re-enable it at any time using Config:Memory cards from the main menu."
					) , slot//TODO: translate internal slot index to human-readable slot description
		);
	}
};

uint FileMcd_GetMtapPort(uint slot)
{
	switch( slot )
	{
		case 0: case 2: case 3: case 4: return 0;
		case 1: case 5: case 6: case 7: return 1;

		jNO_DEFAULT
	}

	return 0;		// technically unreachable.
}

// Returns the multitap slot number, range 1 to 3 (slot 0 refers to the standard
// 1st and 2nd player slots).
uint FileMcd_GetMtapSlot(uint slot)
{
	switch( slot )
	{
		case 0: case 1:
			pxFailDev( "Invalid parameter in call to GetMtapSlot -- specified slot is one of the base slots, not a Multitap slot." );
		break;

		case 2: case 3: case 4: return slot-1;
		case 5: case 6: case 7: return slot-4;

		jNO_DEFAULT
	}

	return 0;		// technically unreachable.
}

bool FileMcd_IsMultitapSlot( uint slot )
{
	return (slot > 1);
}
/*
wxFileName FileMcd_GetSimpleName(uint slot)
{
	if( FileMcd_IsMultitapSlot(slot) )
		return g_Conf->Folders.MemoryCards + wxsFormat( L"Mcd-Multitap%u-Slot%02u.ps2", FileMcd_GetMtapPort(slot)+1, FileMcd_GetMtapSlot(slot)+1 );
	else
		return g_Conf->Folders.MemoryCards + wxsFormat( L"Mcd%03u.ps2", slot+1 );
}
*/
wxString FileMcd_GetDefaultName(uint slot)
{
	if( FileMcd_IsMultitapSlot(slot) )
		return wxsFormat( L"Mcd-Multitap%u-Slot%02u.ps2", FileMcd_GetMtapPort(slot)+1, FileMcd_GetMtapSlot(slot)+1 );
	else
		return wxsFormat( L"Mcd%03u.ps2", slot+1 );
}

FileMemoryCard::FileMemoryCard()
{
	memset8<0xff>( m_effeffs );
	m_chkaddr = 0;
}

void FileMemoryCard::Open()
{
	for( int slot=0; slot<8; ++slot )
	{
		if( FileMcd_IsMultitapSlot(slot) )
		{
			if( !EmuConfig.MultitapPort0_Enabled && (FileMcd_GetMtapPort(slot) == 0) ) continue;
			if( !EmuConfig.MultitapPort1_Enabled && (FileMcd_GetMtapPort(slot) == 1) ) continue;
		}

		wxFileName fname( g_Conf->FullpathToMcd( slot ) );
		wxString str( fname.GetFullPath() );
		bool cont = false;

		if( fname.GetFullName().IsEmpty() )
		{
			str = L"[empty filename]";
			cont = true;
		}

		if( !g_Conf->Mcd[slot].Enabled )
		{
			str = L"[disabled]";
			cont = true;
		}

		if ( g_Conf->Mcd[slot].Type != MemoryCardType::MemoryCard_File ) {
			str = L"[is not memcard file]";
			cont = true;
		}

		Console.WriteLn( cont ? Color_Gray : Color_Green, L"McdSlot %u [File]: " + str, slot );
		if( cont ) continue;

		const wxULongLong fsz = fname.GetSize();
		if( (fsz == 0) || (fsz == wxInvalidSize) )
		{
			// FIXME : Ideally this should prompt the user for the size of the
			// memory card file they would like to create, instead of trying to
			// create one automatically.
		
			if( !Create( str, 8 ) )
			{
#ifndef __LIBRETRO__
				Msgbox::Alert(
					wxsFormat(_( "Could not create a memory card: \n\n%s\n\n" ), str.c_str()) +
					GetDisabledMessage( slot )
				);
#endif
			}
		}

		// [TODO] : Add memcard size detection and report it to the console log.
		//   (8MB, 256Mb, formatted, unformatted, etc ...)

#ifdef __WXMSW__
		NTFS_CompressFile( str, g_Conf->McdCompressNTFS );
#endif

		if( !m_file[slot].Open( str.c_str(), L"r+b" ) )
		{
#ifndef __LIBRETRO__
			// Translation note: detailed description should mention that the memory card will be disabled
			// for the duration of this session.
			Msgbox::Alert(
				wxsFormat(_( "Access denied to memory card: \n\n%s\n\n" ), str.c_str()) +
				GetDisabledMessage( slot )
			);
#endif
		}
		else // Load checksum
		{
			m_ispsx[slot] = m_file[slot].Length() == 0x20000;
			m_chkaddr = 0x210;

			if(!m_ispsx[slot] && !!m_file[slot].Seek( m_chkaddr ))
				m_file[slot].Read( &m_chksum[slot], 8 );
		}
	}
}

void FileMemoryCard::Close()
{
	for( int slot=0; slot<8; ++slot )
	{
		if (m_file[slot].IsOpened()) {
			// Store checksum
			if(!m_ispsx[slot] && !!m_file[slot].Seek(  m_chkaddr ))
				m_file[slot].Write( &m_chksum[slot], 8 );

			m_file[slot].Close();
		}
	}
}

// Returns FALSE if the seek failed (is outside the bounds of the file).
bool FileMemoryCard::Seek( wxFFile& f, u32 adr )
{
	const u32 size = f.Length();

	// If anyone knows why this filesize logic is here (it appears to be related to legacy PSX
	// cards, perhaps hacked support for some special emulator-specific memcard formats that
	// had header info?), then please replace this comment with something useful.  Thanks!  -- air

	u32 offset = 0;

	if( size == MCD_SIZE + 64 )
		offset = 64;
	else if( size == MCD_SIZE + 3904 )
		offset = 3904;
	else
	{
		// perform sanity checks here?
	}

	return f.Seek( adr + offset );
}

// returns FALSE if an error occurred (either permission denied or disk full)
bool FileMemoryCard::Create( const wxString& mcdFile, uint sizeInMB )
{
	//int enc[16] = {0x77,0x7f,0x7f,0x77,0x7f,0x7f,0x77,0x7f,0x7f,0x77,0x7f,0x7f,0,0,0,0};

	Console.WriteLn( L"(FileMcd) Creating new %uMB memory card: " + mcdFile, sizeInMB );

	wxFFile fp( mcdFile, L"wb" );
	if( !fp.IsOpened() ) return false;

	for( uint i=0; i<(MC2_MBSIZE*sizeInMB)/sizeof(m_effeffs); i++ )
	{
		if( fp.Write( m_effeffs, sizeof(m_effeffs) ) == 0 )
			return false;
	}
	return true;
}

s32 FileMemoryCard::IsPresent( uint slot )
{
	return m_file[slot].IsOpened();
}

void FileMemoryCard::GetSizeInfo( uint slot, PS2E_McdSizeInfo& outways )
{
	outways.SectorSize				= 512; // 0x0200
	outways.EraseBlockSizeInSectors	= 16;  // 0x0010
	outways.Xor						= 18;  // 0x12, XOR 02 00 00 10

	if( pxAssert( m_file[slot].IsOpened() ) )
		outways.McdSizeInSectors	= m_file[slot].Length() / (outways.SectorSize + outways.EraseBlockSizeInSectors);
	else
		outways.McdSizeInSectors	= 0x4000;

	u8 *pdata = (u8*)&outways.McdSizeInSectors;
	outways.Xor ^= pdata[0] ^ pdata[1] ^ pdata[2] ^ pdata[3];
}

bool FileMemoryCard::IsPSX( uint slot )
{
	return m_ispsx[slot];
}

s32 FileMemoryCard::Read( uint slot, u8 *dest, u32 adr, int size )
{
	wxFFile& mcfp( m_file[slot] );
	if( !mcfp.IsOpened() )
	{
		DevCon.Error( "(FileMcd) Ignoring attempted read from disabled slot." );
		memset(dest, 0, size);
		return 1;
	}
	if( !Seek(mcfp, adr) ) return 0;
	return mcfp.Read( dest, size ) != 0;
}

s32 FileMemoryCard::Save( uint slot, const u8 *src, u32 adr, int size )
{
	wxFFile& mcfp( m_file[slot] );

	if( !mcfp.IsOpened() )
	{
		DevCon.Error( "(FileMcd) Ignoring attempted save/write to disabled slot." );
		return 1;
	}

	if(m_ispsx[slot])
	{
		m_currentdata.MakeRoomFor( size );
		for (int i=0; i<size; i++) m_currentdata[i] = src[i];
	}
	else
	{
		if( !Seek(mcfp, adr) ) return 0;
		m_currentdata.MakeRoomFor( size );
		mcfp.Read( m_currentdata.GetPtr(), size);
		

		for (int i=0; i<size; i++)
		{
			if ((m_currentdata[i] & src[i]) != src[i])
				Console.Warning("(FileMcd) Warning: writing to uncleared data. (%d) [%08X]", slot, adr);
			m_currentdata[i] &= src[i];
		}

		// Checksumness
		{
			if(adr == m_chkaddr) 
				Console.Warning("(FileMcd) Warning: checksum sector overwritten. (%d)", slot);

			u64 *pdata = (u64*)&m_currentdata[0];
			u32 loops = size / 8;

			for(u32 i = 0; i < loops; i++)
				m_chksum[slot] ^= pdata[i];
		}
	}

	if( !Seek(mcfp, adr) ) return 0;

	int status = mcfp.Write( m_currentdata.GetPtr(), size );

	if( status ) {
		static auto last = std::chrono::time_point<std::chrono::system_clock>();

		std::chrono::duration<float> elapsed = std::chrono::system_clock::now() - last;
		if(elapsed > std::chrono::seconds(5)) {
			wxString name, ext;
			wxFileName::SplitPath(m_file[slot].GetName(), NULL, NULL, &name, &ext);
			OSDlog( Color_StrongYellow, false, "Memory Card %s written.", (const char *)(name + "." + ext).c_str() );
			last = std::chrono::system_clock::now();
		}
		return 1;
	}

	return 0;
}

s32 FileMemoryCard::EraseBlock( uint slot, u32 adr )
{
	wxFFile& mcfp( m_file[slot] );

	if( !mcfp.IsOpened() )
	{
		DevCon.Error( "MemoryCard: Ignoring erase for disabled slot." );
		return 1;
	}

	if( !Seek(mcfp, adr) ) return 0;
	return mcfp.Write( m_effeffs, sizeof(m_effeffs) ) != 0;
}

u64 FileMemoryCard::GetCRC( uint slot )
{
	wxFFile& mcfp( m_file[slot] );
	if( !mcfp.IsOpened() ) return 0;

	u64 retval = 0;

	if(m_ispsx[slot])
	{
		if( !Seek( mcfp, 0 ) ) return 0;

		// Process the file in 4k chunks.  Speeds things up significantly.
	
		u64 buffer[528*8];		// use 528 (sector size), ensures even divisibility
	
		const uint filesize = mcfp.Length() / sizeof(buffer);
		for( uint i=filesize; i; --i )
		{
			mcfp.Read( &buffer, sizeof(buffer) );
			for( uint t=0; t<ArraySize(buffer); ++t )
				retval ^= buffer[t];
		}
	}
	else
	{
		retval = m_chksum[slot];
	}

	return retval;
}

// --------------------------------------------------------------------------------------
//  MemoryCard Component API Bindings
// --------------------------------------------------------------------------------------

struct Component_FileMcd
{
	PS2E_ComponentAPI_Mcd      api;  // callbacks the plugin provides back to the emulator
	FileMemoryCard             impl; // class-based implementations we refer to when API is invoked
	FolderMemoryCardAggregator implFolder;

	Component_FileMcd();
};

uint FileMcd_ConvertToSlot( uint port, uint slot )
{
	if( slot == 0 ) return port;
	if( port == 0 ) return slot+1;		// multitap 1
	return slot + 4;					// multitap 2
}

static void PS2E_CALLBACK FileMcd_EmuOpen( PS2E_THISPTR thisptr, const PS2E_SessionInfo *session )
{
	// detect inserted memory card types
	for ( uint slot = 0; slot < 8; ++slot ) {
		if ( g_Conf->Mcd[slot].Enabled ) {
			MemoryCardType type = MemoryCardType::MemoryCard_File; // default to file if we can't find anything at the path so it gets auto-generated

			const wxString path = g_Conf->FullpathToMcd( slot );
			if ( wxFileExists( path ) ) {
				type = MemoryCardType::MemoryCard_File;
			} else if ( wxDirExists( path ) ) {
				type = MemoryCardType::MemoryCard_Folder;
			}

			g_Conf->Mcd[slot].Type = type;
		}
	}

	thisptr->impl.Open();
	thisptr->implFolder.SetFiltering( g_Conf->EmuOptions.McdFolderAutoManage );
	thisptr->implFolder.Open();
}

static void PS2E_CALLBACK FileMcd_EmuClose( PS2E_THISPTR thisptr )
{
	thisptr->implFolder.Close();
	thisptr->impl.Close();
}

static s32 PS2E_CALLBACK FileMcd_IsPresent( PS2E_THISPTR thisptr, uint port, uint slot )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.IsPresent( combinedSlot );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.IsPresent( combinedSlot );
	default:
		return false;
	}
}

static void PS2E_CALLBACK FileMcd_GetSizeInfo( PS2E_THISPTR thisptr, uint port, uint slot, PS2E_McdSizeInfo* outways )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		thisptr->impl.GetSizeInfo( combinedSlot, *outways );
		break;
	case MemoryCardType::MemoryCard_Folder:
		thisptr->implFolder.GetSizeInfo( combinedSlot, *outways );
		break;
	default:
		return;
	}
}

static bool PS2E_CALLBACK FileMcd_IsPSX( PS2E_THISPTR thisptr, uint port, uint slot )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.IsPSX( combinedSlot );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.IsPSX( combinedSlot );
	default:
		return false;
	}
}

static s32 PS2E_CALLBACK FileMcd_Read( PS2E_THISPTR thisptr, uint port, uint slot, u8 *dest, u32 adr, int size )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.Read( combinedSlot, dest, adr, size );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.Read( combinedSlot, dest, adr, size );
	default:
		return 0;
	}
}

static s32 PS2E_CALLBACK FileMcd_Save( PS2E_THISPTR thisptr, uint port, uint slot, const u8 *src, u32 adr, int size )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.Save( combinedSlot, src, adr, size );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.Save( combinedSlot, src, adr, size );
	default:
		return 0;
	}
}

static s32 PS2E_CALLBACK FileMcd_EraseBlock( PS2E_THISPTR thisptr, uint port, uint slot, u32 adr )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.EraseBlock( combinedSlot, adr );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.EraseBlock( combinedSlot, adr );
	default:
		return 0;
	}
}

static u64 PS2E_CALLBACK FileMcd_GetCRC( PS2E_THISPTR thisptr, uint port, uint slot )
{
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	case MemoryCardType::MemoryCard_File:
		return thisptr->impl.GetCRC( combinedSlot );
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.GetCRC( combinedSlot );
	default:
		return 0;
	}
}

static void PS2E_CALLBACK FileMcd_NextFrame( PS2E_THISPTR thisptr, uint port, uint slot ) {
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	//case MemoryCardType::MemoryCard_File:
	//	thisptr->impl.NextFrame( combinedSlot );
	//	break;
	case MemoryCardType::MemoryCard_Folder:
		thisptr->implFolder.NextFrame( combinedSlot );
		break;
	default:
		return;
	}
}

static bool PS2E_CALLBACK FileMcd_ReIndex( PS2E_THISPTR thisptr, uint port, uint slot, const wxString& filter ) {
	const uint combinedSlot = FileMcd_ConvertToSlot( port, slot );
	switch ( g_Conf->Mcd[combinedSlot].Type ) {
	//case MemoryCardType::MemoryCard_File:
	//	return thisptr->impl.ReIndex( combinedSlot, filter );
	//	break;
	case MemoryCardType::MemoryCard_Folder:
		return thisptr->implFolder.ReIndex( combinedSlot, g_Conf->EmuOptions.McdFolderAutoManage, filter );
		break;
	default:
		return false;
	}
}

Component_FileMcd::Component_FileMcd()
{
	memzero( api );

	api.Base.EmuOpen	= FileMcd_EmuOpen;
	api.Base.EmuClose	= FileMcd_EmuClose;

	api.McdIsPresent	= FileMcd_IsPresent;
	api.McdGetSizeInfo	= FileMcd_GetSizeInfo;
	api.McdIsPSX		= FileMcd_IsPSX;
	api.McdRead			= FileMcd_Read;
	api.McdSave			= FileMcd_Save;
	api.McdEraseBlock	= FileMcd_EraseBlock;
	api.McdGetCRC		= FileMcd_GetCRC;
	api.McdNextFrame    = FileMcd_NextFrame;
	api.McdReIndex      = FileMcd_ReIndex;
}


// --------------------------------------------------------------------------------------
//  Library API Implementations
// --------------------------------------------------------------------------------------
static const char* PS2E_CALLBACK FileMcd_GetName()
{
	return "PlainJane Mcd";
}

static const PS2E_VersionInfo* PS2E_CALLBACK FileMcd_GetVersion( u32 component )
{
	static const PS2E_VersionInfo version = { 0,1,0, SVN_REV };
	return &version;
}

static s32 PS2E_CALLBACK FileMcd_Test( u32 component, const PS2E_EmulatorInfo* xinfo )
{
	if( component != PS2E_TYPE_Mcd ) return 0;

	// Check and make sure the user has a hard drive?
	// Probably not necessary :p
	return 1;
}

static PS2E_THISPTR PS2E_CALLBACK FileMcd_NewComponentInstance( u32 component )
{
	if( component != PS2E_TYPE_Mcd ) return NULL;

	try
	{
		return new Component_FileMcd();
	}
	catch( std::bad_alloc& )
	{
		Console.Error( "Allocation failed on Component_FileMcd! (out of memory?)" );
	}
	return NULL;
}

static void PS2E_CALLBACK FileMcd_DeleteComponentInstance( PS2E_THISPTR instance )
{
	delete instance;
}

static void PS2E_CALLBACK FileMcd_SetSettingsFolder( const char* folder )
{
}

static void PS2E_CALLBACK FileMcd_SetLogFolder( const char* folder )
{
}

static const PS2E_LibraryAPI FileMcd_Library =
{
	FileMcd_GetName,
	FileMcd_GetVersion,
	FileMcd_Test,
	FileMcd_NewComponentInstance,
	FileMcd_DeleteComponentInstance,
	FileMcd_SetSettingsFolder,
	FileMcd_SetLogFolder
};

// If made into an external plugin, this function should be renamed to PS2E_InitAPI, so that
// PCSX2 can find the export in the expected location.
extern "C" const PS2E_LibraryAPI* FileMcd_InitAPI( const PS2E_EmulatorInfo* emuinfo )
{
	return &FileMcd_Library;
}

//Tests if a string is a valid name for a new file within a specified directory.
//returns true if:
//     - the file name has a minimum length of minNumCharacters chars (default is 5 chars: at least 1 char + '.' + 3-chars extension)
// and - the file name is within the basepath directory (doesn't contain .. , / , \ , etc)
// and - file name doesn't already exist
// and - can be created on current system (it is actually created and deleted for this test).
bool isValidNewFilename( wxString filenameStringToTest, wxDirName atBasePath, wxString& out_errorMessage, uint minNumCharacters)
{
	if ( filenameStringToTest.Length()<1 || filenameStringToTest.Length()<minNumCharacters )
	{
		out_errorMessage = _("File name empty or too short");
		return false;
	}

	if( (atBasePath + wxFileName(filenameStringToTest)).GetFullPath() != (atBasePath + wxFileName(filenameStringToTest).GetFullName()).GetFullPath() ){
		out_errorMessage = _("File name outside of required directory");
		return false;
	}

	if ( wxFileExists( (atBasePath + wxFileName(filenameStringToTest)).GetFullPath() ))
	{
		out_errorMessage = _("File name already exists");
		return false;
	}
	if ( wxDirExists( (atBasePath + wxFileName(filenameStringToTest)).GetFullPath() ))
	{
		out_errorMessage = _( "File name already exists" );
		return false;
	}

	wxFile fp;
	if( !fp.Create( (atBasePath + wxFileName(filenameStringToTest)).GetFullPath() ))
	{
		out_errorMessage = _("The Operating-System prevents this file from being created");
		return false;
	}
	fp.Close();
	wxRemoveFile( (atBasePath + wxFileName(filenameStringToTest)).GetFullPath() );

	out_errorMessage = L"[OK - New file name is valid]";  //shouldn't be displayed on success, hence not translatable.
	return true;
}