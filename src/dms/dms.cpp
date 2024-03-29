/*******************************************************************************
   Copyright (C) 2013 SequoiaDB Software Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.
*******************************************************************************/

#include "dms.hpp"
#include "pd.hpp"
using namespace bson ;

const char *gKeyFieldName = DMS_KEY_FIELDNAME ;

dmsFile::dmsFile () :
_header(NULL),
_pFileName(NULL)
{
}

dmsFile::~dmsFile ()
{
   if ( _pFileName )
   {
      free ( _pFileName ) ;
   }
   close () ;
}

int dmsFile::insert ( BSONObj &record, BSONObj &outRecord, dmsRecordID &rid )
{
   int rc                     = EDB_OK ;
   PAGEID pageID              = 0 ;
   char *page                 = NULL ;
   dmsPageHeader *pageHeader  = NULL ;
   int recordSize             = 0 ;
   SLOTOFF offsetTemp         = 0 ;
   const char *pGKeyFieldName = NULL ;
   dmsRecord recordHeader ;
   bool isReusedSlot          = false ;
   SLOTOFF reuseSlot          = 0 ;
   recordSize                 = record.objsize() ;

   // when we attempt to insert record, first we have to verify it include _id field
   if ( recordSize > DMS_MAX_RECORD )
   {
      rc = EDB_INVALIDARG ;
      PD_LOG ( PDERROR, "record cannot bigger than 4MB" ) ;
      goto error ;
   }
   pGKeyFieldName = gKeyFieldName ;

   // make sure _id exists
   if ( record.getFieldDottedOrArray ( pGKeyFieldName ).eoo () )
   {
      rc = EDB_INVALIDARG ;
      PD_LOG ( PDERROR, "record must be with _id" ) ;
      goto error ;
   }
   
retry :
   // lock the database
   _mutex.get() ;
   // and then we should get the required record size 
   pageID = _findPage ( recordSize + sizeof(dmsRecord) ) ;
   // if there's not enough space in any existing pages, let's release db lock
   if ( DMS_INVALID_PAGEID == pageID )
   {
      _mutex.release () ;
      // if there's not enough space in any existing pages, let's release db lock and
      // try to allocate a new segment by calling _extendSegment
      if ( _extendMutex.try_get() )
      {
         // calling _extendSegment
         rc = _extendSegment () ;
         if ( rc )
         {
             PD_LOG ( PDERROR, "Failed to extend segment, rc = %d", rc ) ;
             _extendMutex.release () ;
             goto error ;
         }
      }
      else
      {
         // if we cannot get the extendmutex, that means someone else is trying to extend 
         // so let's wait until getting the mutex, and release it and try again
         _extendMutex.get() ;
      }
      _extendMutex.release () ;
      goto retry ;
   }
   // find the in-memory offset for the page
   page = pageToOffset ( pageID ) ;
   // if something wrong, let's return error
   if ( !page )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Failed to find the page" ) ;
      goto error_releasemutex ;
   }
   // set page header
   pageHeader = (dmsPageHeader *)page ;
   if ( memcmp ( pageHeader->_eyeCatcher, DMS_PAGE_EYECATCHER,
                 DMS_PAGE_EYECATCHER_LEN ) != 0 )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Invalid page header" ) ;
      goto error_releasemutex ;
   }
   // slot offset is the last byte of slots
   // free offset is the first byte of data
   // so freeOffset - slotOffset is the actual free space excluding holes
   if (
      // make sure there's still holes to recover
      ( pageHeader->_freeSpace >
        pageHeader->_freeOffset - pageHeader->_slotOffset ) &&
      // if there's no free space excluding holes
      ( pageHeader->_slotOffset + recordSize + sizeof(dmsRecord) + sizeof(SLOTID) > pageHeader->_freeOffset )
   )
   {
      // recover empty hole from page
      _recoverSpace ( page ) ;
   }
   if ( 
      // make sure there is enough free space
      ( pageHeader->_freeSpace < recordSize + sizeof(dmsRecord) + sizeof(SLOTID) ) || 
      ( pageHeader->_freeOffset - pageHeader->_slotOffset <  
        recordSize + sizeof(dmsRecord) + sizeof(SLOTID) )
   )
   {
      PD_LOG ( PDERROR, "Something big wrong!!" ) ;
      rc = EDB_SYS ;
      goto error_releasemutex ;
   }
   offsetTemp = pageHeader->_freeOffset - recordSize - sizeof(dmsRecord) ;
   recordHeader._size = recordSize + sizeof( dmsRecord ) ;
   recordHeader._flag = DMS_RECORD_FLAG_NORMAL ;
   // copy the slot
   if ( DMS_REUSE_SLOT_EMPTY ==  pageHeader->_reuseSlotOffset )
   {
      *(SLOTOFF*)( page + sizeof(dmsPageHeader) +
                   pageHeader->_numSlots * sizeof(SLOTOFF) ) = offsetTemp ;
   } else
   {
      reuseSlot = pageHeader->_reuseSlotOffset ;
      pageHeader->_reuseSlotOffset = *(SLOTOFF*)( page + sizeof(dmsPageHeader) +
                                        pageHeader->_reuseSlotOffset * sizeof(SLOTOFF)  ) ;
      *(SLOTOFF*)( page + sizeof(dmsPageHeader) + reuseSlot * sizeof(SLOTOFF) ) = offsetTemp ;
      isReusedSlot = true ;
   }     
 // copy the record header
   memcpy ( page + offsetTemp, (char*)&recordHeader, sizeof(dmsRecord) ) ;
   // copy the record body
   memcpy ( page + offsetTemp + sizeof(dmsRecord),
            record.objdata(),
            recordSize ) ;
   outRecord  = BSONObj ( page + offsetTemp + sizeof(dmsRecord) ) ;
   rid._pageID = pageID ;
   rid._slotID = isReusedSlot ? reuseSlot : pageHeader->_numSlots ;
   // modify metadata in page
   if ( !isReusedSlot )
   {
      pageHeader->_numSlots ++ ;
      pageHeader->_slotOffset += sizeof(SLOTID) ;
   }
   pageHeader->_freeOffset = offsetTemp ;
   // modify database metadata
   _updateFreeSpace ( pageHeader,
                      -(recordSize+sizeof(SLOTID)+sizeof(dmsRecord)),
                      pageID ) ;
   _mutex.release () ;  
done :
   return rc ;
error_releasemutex :
   _mutex.release() ;
error :
   goto done ;
}

int dmsFile::remove ( dmsRecordID &rid )
{
   int rc                     = EDB_OK ;
   SLOTOFF slot               = 0 ;
   char *page                 = NULL ;
   dmsRecord *recordHeader    = NULL ;
   dmsPageHeader *pageHeader  = NULL ;
   std::pair<std::multimap<unsigned int, PAGEID>::iterator,
             std::multimap<unsigned int, PAGEID>::iterator> ret ;
   _mutex.get () ;
   // find the page in memory
   page = pageToOffset ( rid._pageID ) ;
   if ( !page )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Failed to find the page for %u; %u",
               rid._pageID, rid._slotID ) ;
      goto error ;
   }
   // search the given slot
   rc = _searchSlot ( page, rid, slot ) ;
   if ( rc )
   {
      PD_LOG ( PDERROR, "Failed to search slot, rc = %d", rc ) ;
      goto error ;
   }
   if ( DMS_SLOT_EMPTY == slot )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "The record is droped" ) ;
      goto error ;
   }
   // set page header
   pageHeader = (dmsPageHeader *)page ;
   // set slot to empty
   *(SLOTID*)(page + sizeof( dmsPageHeader ) +
              rid._slotID * sizeof(SLOTID)) = DMS_SLOT_EMPTY ;
   // set record header
   recordHeader = (dmsRecord *)(page+slot) ;
   recordHeader->_flag = DMS_RECORD_FLAG_DROPPED ;
   // update database metadata
   _updateFreeSpace ( pageHeader, recordHeader->_size, rid._pageID ) ;
done :
   _mutex.release () ;
   return rc ;
error :
   goto done ; 
}

int dmsFile::find ( dmsRecordID &rid, BSONObj &result )
{
   int rc                 = EDB_OK ;
   SLOTID slot            = 0 ;
   char *page             = NULL ;
   dmsRecord *recordHeader= NULL ;
   dmsPageHeader *pageHeader  = NULL ;

   // S lock the database
   _mutex.get_shared () ;
   // goto the page and verify the slot is valid 
   page = pageToOffset ( rid._pageID ) ;

   if ( !page )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Failed to find the page" ) ;
      goto error ;
   }

   pageHeader = (dmsPageHeader *)page ;

   rc = _searchSlot ( page, rid, slot ) ;
   if ( rc ) 
   {
      PD_LOG ( PDERROR, "Failed to search slot rc = %d", rc ) ;
      goto error ;
   }
   // if slot is empty , something big wrong
   if ( DMS_SLOT_EMPTY == slot ||  pageHeader->_numSlots > slot )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "The record is dropped" ) ;
      goto error ;
   }
   // ger the record header
   recordHeader = (dmsRecord *)( page + slot ) ;
   // if recordheader->_flag is dropped , this record is droped already
   if ( DMS_RECORD_FLAG_DROPPED == recordHeader->_flag )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "This data is dropped" ) ;
      goto error ;
   }
   result = BSONObj ( page + slot + sizeof(dmsRecord) ).copy () ;
done :
   _mutex.release_shared () ;
   return rc ;
error :
   goto done ;
}

void dmsFile::_updateFreeSpace ( dmsPageHeader *header, int changeSize,
                                 PAGEID pageID )
{
   unsigned int freeSpace = header->_freeSpace ;
   std::pair<std::multimap<unsigned int, PAGEID>::iterator,
             std::multimap<unsigned int, PAGEID>::iterator> ret ;
   ret = _freeSpaceMap.equal_range ( freeSpace ) ;
   for ( std::multimap < unsigned int, PAGEID>::iterator it = ret.first ;
         it != ret.second; ++it )
   {
      if ( it->second == pageID )
      {
         _freeSpaceMap.erase ( it ) ;
         break ;
      }
   }
   // increase page free space
   freeSpace += changeSize ;
   header->_freeSpace = freeSpace ;
   // insert into free space map
   _freeSpaceMap.insert ( 
         pair<unsigned int, PAGEID>(freeSpace, pageID) ) ;
}

int dmsFile::initialize ( const char *pFileName )
{
   offsetType offset = 0 ;
   int rc = EDB_OK ;
   // duplicate file name
   _pFileName = strdup ( pFileName ) ;
   if ( !_pFileName )
   {
      rc = EDB_OOM ;
      PD_LOG ( PDERROR, "Failed to duplicate file name" ) ;
      goto error ;
   }
 
   // open file
   rc = open ( _pFileName, OSS_PRIMITIVE_FILE_OP_OPEN_ALWAYS ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to open file %s, rc = %d",
                 _pFileName, rc ) ;
getfilesize:
   // get file size
   rc = _fileOp.getSize ( &offset ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to get file size, rc = %d",
                 rc ) ;
   // if file size is 0, that means it's newly created file and we should initailize it
   if ( !offset )
   {
      rc = _initNew () ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to initialize file, rc = %d",
                    rc ) ;
      goto getfilesize ;
   }
   // load data
   rc = _loadData () ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to load data, rc = %d", rc ) ;
done :
   return rc ;
error :
   goto done ;
}

// caller must hold extend latch
int dmsFile::_extendSegment ()
{
   // extend a new segment
   int rc              = EDB_OK ;
   char *data          = NULL ;
   int freeMapSize     = 0 ;
   dmsPageHeader pageHeader ;
   offsetType offset   = 0 ;

   // first let's get the size of file before extend
   rc = _fileOp.getSize ( &offset ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to get file size, rc = %d", rc ) ;
   
   // extend the file 
   rc = _extendFile ( DMS_FILE_SEGMENT_SIZE ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to extend segment rc = %d", rc ) ;

   // map from original end to new end 
   rc = map ( offset, DMS_FILE_SEGMENT_SIZE, ( void ** )&data ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to map file, rc = %d", rc ) ;

   // create page header structure and we are going to copy to each page
   strcpy ( pageHeader._eyeCatcher, DMS_PAGE_EYECATCHER ) ;
   pageHeader._size = DMS_PAGESIZE ;
   pageHeader._flag = DMS_PAGE_FLAG_NORMAL ;
   pageHeader._numSlots = 0 ;
   pageHeader._slotOffset = sizeof ( dmsPageHeader ) ;
   pageHeader._freeSpace = DMS_PAGESIZE - sizeof(dmsPageHeader) ;
   pageHeader._freeOffset = DMS_PAGESIZE ;
   pageHeader._reuseSlotOffset = DMS_REUSE_SLOT_EMPTY ;
   // copy header to each page
   for ( int i = 0; i < DMS_FILE_SEGMENT_SIZE; i+= DMS_PAGESIZE )
   {
       memcpy ( data +  i, (char*)&pageHeader, sizeof(dmsPageHeader) ) ;
   }

   // free space handling
   freeMapSize = _freeSpaceMap.size () ;
   // insert into free space map
   for ( int i = 0; i < DMS_PAGES_PER_SEGMENT; ++i )
   {
      _freeSpaceMap.insert ( pair<unsigned int, PAGEID>
                              (pageHeader._freeSpace,
                                i+freeMapSize ) ) ;
   }
   // push the segment into body list
   _body.push_back ( data ) ;
   _header->_size += DMS_PAGES_PER_SEGMENT ;
done :
   return rc ;
error :
   goto done ;
}

int dmsFile::_initNew ()
{
   // initialize a newly created fiel, let's append DMS_FILE_HEADER_SIZE bytes and the initialize the header
   int rc = EDB_OK ;
   rc = _extendFile ( DMS_FILE_HEADER_SIZE ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to extend file, rc = %d", rc ) ;
   rc = map ( 0, DMS_FILE_HEADER_SIZE, ( void ** )&_header ) ;
   PD_RC_CHECK ( rc, PDERROR, "Failed to map, rc = %d", rc ) ;

   strcpy ( _header->_eyeCatcher, DMS_HEADER_EYECATCHER ) ;
   _header->_size = 0 ;
   _header->_flag = DMS_HEADER_FLAG_NORMAL ;
   _header->_version = DMS_HEADER_VERSION_CURRENT ;
done :
   return rc ;
error :
   goto done ;
}

PAGEID dmsFile::_findPage ( size_t requiredSize )
{
   std::multimap<unsigned int, PAGEID>::iterator findIter ;
   findIter = _freeSpaceMap.upper_bound ( requiredSize ) ;
   if ( findIter != _freeSpaceMap.end() )
   {
      return findIter->second ;
   }
   return DMS_INVALID_PAGEID ;
}

int dmsFile::_extendFile ( int size )
{
   int rc = EDB_OK ;
   char temp[DMS_EXTEND_SIZE] = {0} ;
   memset ( temp, 0, DMS_EXTEND_SIZE ) ;
   if ( size % DMS_EXTEND_SIZE != 0 )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Invalid extend size, must be multiple of %d",
               DMS_EXTEND_SIZE ) ;
      goto error ;
   }
   // write file 
   for ( int i = 0; i < size; i += DMS_EXTEND_SIZE )
   {
      _fileOp.seekToEnd () ;
      rc = _fileOp.Write ( temp, DMS_EXTEND_SIZE ) ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to write to file, rc = %d", rc ) ;
   }
done :
   return rc ;
error :
   goto done ;
}

int dmsFile::_searchSlot ( char *page, dmsRecordID &rid,
                           SLOTOFF &slot )
{
   int rc = EDB_OK ;
   dmsPageHeader *pageHeader = NULL ;
   if ( !page )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "page is NULL" ) ;
      goto error ;
   }
   // let's first verify the rid is valid
   if ( 0 > rid._pageID || 0 > rid._slotID )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Invalid RID: %d. %d",
               rid._pageID, rid._slotID ) ;
      goto error ;
   }
   pageHeader = (dmsPageHeader *)page ;
   if ( rid._slotID > pageHeader->_numSlots )
   {
      rc = EDB_SYS ; 
      PD_LOG ( PDERROR, "Slot is out of range, provided: %d, max: %d",
               rid._slotID, pageHeader->_numSlots ) ;
      goto error ;
   }
   slot = *(SLOTOFF*)(page + sizeof(dmsPageHeader)+
                      rid._slotID*sizeof(SLOTOFF) ) ;
done :
   return rc ;
error :
   goto done ;
}

int dmsFile::_loadData ()
{
   int rc                    = EDB_OK ;
   int numPage               = 0 ;
   int numSegments           = 0 ;
   dmsPageHeader *pageHeader = NULL ;
   char *data                = NULL ;
   dmsRecordID recordID ;
   SLOTID slotID             = 0;
   BSONObj bson ;

   // check if header is valid
   if ( !_header )
   {
      rc = map ( 0, DMS_FILE_HEADER_SIZE, ( void ** )&_header ) ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to map file header, rc = %d", rc ) ;
   }
   numPage = _header->_size ;
   if ( numPage % DMS_PAGES_PER_SEGMENT )
   {
      rc = EDB_SYS ;
      PD_LOG ( PDERROR, "Failed to load data, partial segments detected" ) ;
      goto error ;
   }
   numSegments = numPage / DMS_PAGES_PER_SEGMENT ;
   // get the segents number
   if ( numSegments > 0 )
   {
      for ( int i = 0; i < numSegments; ++i )
      {
         // map each segment into memory
         rc = map ( DMS_FILE_HEADER_SIZE + DMS_FILE_SEGMENT_SIZE * i,
                    DMS_FILE_SEGMENT_SIZE, ( void **)&data ) ;
         PD_RC_CHECK ( rc, PDERROR, "Failed to map segment %d, rc = %d",
                       i, rc ) ;
         _body.push_back ( data ) ;
         // initialize each page into freeSpaceMap
         for ( unsigned int k = 0; k < DMS_PAGES_PER_SEGMENT; ++ k )
         {
            pageHeader = ( dmsPageHeader * ) ( data + k*DMS_PAGESIZE ) ;
            _freeSpaceMap.insert (
                  pair<unsigned int, PAGEID>(pageHeader->_freeSpace, k ) ) ;
            slotID = ( SLOTID ) pageHeader->_numSlots ;
            recordID._pageID = (PAGEID) k ;
            // for each record in the page, let's insert into index
         }
       } //for ( int i = 0; i < numSegments; ++i )
   }// if ( numSegments > 0 )
done : 
   return rc ;
error :
   goto done ;
}

void Qsort(SLOTID  myArray[], SLOTID  min,SLOTID  max) ;

void dmsFile::_recoverSpace ( char *page )
{
   char *pLeft               = NULL ;
   char *pRight              = NULL ;
   SLOTOFF slot              = 0 ;
   int recordSize            = 0 ;
   bool isRecover            = false ;
   dmsRecord *recordHeader   = NULL ;   
   dmsPageHeader *pageHeader = NULL ;
   SLOTOFF slotTemp           = 0 ;
   pLeft = page + sizeof(dmsPageHeader) ;
   pRight = page + DMS_PAGESIZE ;

   pageHeader = (dmsPageHeader *)page ;
   
   // the slots order by data space, from the page end 
   SLOTID slotOrder[pageHeader->_numSlots] ;
   
   for ( unsigned int i = 0; i < pageHeader->_numSlots; ++i )
   {
      slotOrder[i] = *(SLOTID*)( page + sizeof( dmsPageHeader ) +
                                  i * sizeof(SLOTID) ) ;
      if ( DMS_SLOT_EMPTY == slotOrder[i] )
      {
         slotTemp = pageHeader->_reuseSlotOffset ;
         pageHeader->_reuseSlotOffset = slotOrder[i] ;
         *(SLOTID*)( page + sizeof( dmsPageHeader ) +
                                  i * sizeof(SLOTID) ) = slotTemp ;
         slotOrder[i] = 0 ;
      } else if ( pageHeader->_numSlots > slotOrder[i] ||
                  DMS_REUSE_SLOT_EMPTY == slotOrder[i] )
      {
         slotOrder[i] = 0 ;
      }
       
   }
   Qsort( slotOrder, 0, pageHeader->_numSlots ) ;
   // recover space
   for ( unsigned int i =  pageHeader->_numSlots; i >=0 && slotOrder[i] > 0 ; --i )
   {
      slot = slotOrder[i] ;
      recordHeader = (dmsRecord *)(page + slot ) ;
      recordSize = recordHeader->_size ;
      pRight -= recordSize ;
      if ( isRecover )
      {
         memmove ( pRight, page + slot, recordSize ) ;
      }
      else if (  pRight - page - slot > 0 )
      {
         isRecover = true ;
      }
      
   }
   pageHeader->_freeOffset = pRight - page ;

}

void Qsort(SLOTID  myArray[], SLOTID  min, SLOTID  max)
{
    SLOTID  pivot = myArray[(min + max) / 2];

    SLOTID  left = min, right = max;

    while (left < right) {
        while (myArray[left] < pivot) {
            left++;
        }
        while (myArray[right] > pivot) {
            right--;
        }

        if (left <= right) {
            SLOTID  temp = myArray[left];
            myArray[left] = myArray[right];
            myArray[right] = temp;
            left++;
            right--;
        }
    }

    if (min < right) {
        Qsort(myArray, min, right);
    }
    if (left < max) {
        Qsort(myArray, left, max);
    }
} 
