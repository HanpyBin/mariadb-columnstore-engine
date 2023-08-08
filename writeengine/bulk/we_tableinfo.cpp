/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/*******************************************************************************
 * $Id: we_tableinfo.cpp 4648 2013-05-29 21:42:40Z rdempsey $
 *
 *******************************************************************************/
/** @file */

#include "we_tableinfo.h"
#include "we_bulkstatus.h"
#include "we_bulkload.h"

#include <sstream>
#include <sys/time.h>
#include <ctime>
#include <unistd.h>
#include <sys/types.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <utility>
// @bug 2099+
#include <iostream>
#include <libmarias3/marias3.h>
#include <string.h>
using namespace std;

// @bug 2099-
#include <boost/filesystem/path.hpp>
using namespace boost;

#include "we_config.h"
#include "we_simplesyslog.h"
#include "we_bulkrollbackmgr.h"
#include "we_confirmhdfsdbfile.h"

#include "querytele.h"
using namespace querytele;

#include "oamcache.h"
#include "cacheutils.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/io/file.h>
#include <parquet/stream_reader.h>
using namespace execplan;

#include "utils_utf8.h"  // utf8_truncate_point()

namespace
{
const std::string BAD_FILE_SUFFIX = ".bad";  // Reject data file suffix
const std::string ERR_FILE_SUFFIX = ".err";  // Job error file suffix
const std::string BOLD_START = "\033[0;1m";
const std::string BOLD_STOP = "\033[0;39m";
}  // namespace

namespace WriteEngine
{
// Helpers
int TableInfo::compareHWMs(const int smallestColumnId, const int widerColumnId,
                           const uint32_t smallerColumnWidth, const uint32_t widerColumnWidth,
                           const std::vector<DBRootExtentInfo>& segFileInfo, int& colIdx)
{
  int rc = NO_ERROR;
  if (widerColumnId < 0)
  {
    return rc;
  }
  uint32_t columnDiffMultiplier = widerColumnWidth / smallerColumnWidth;
  HWM hwmLo = segFileInfo[smallestColumnId].fLocalHwm * columnDiffMultiplier;
  HWM hwmHi = hwmLo + columnDiffMultiplier - 1;

  if ((segFileInfo[widerColumnId].fLocalHwm < hwmLo) || (segFileInfo[widerColumnId].fLocalHwm > hwmHi))
  {
    colIdx = widerColumnId;
    rc = ERR_BRM_HWMS_OUT_OF_SYNC;
  }
  return rc;
}

//------------------------------------------------------------------------------
// Puts the current thread to sleep for the specified number of milliseconds.
// (Ex: used to wait for a Read buffer to become available.)
//------------------------------------------------------------------------------
void TableInfo::sleepMS(long ms)
{
  struct timespec rm_ts;

  rm_ts.tv_sec = ms / 1000;
  rm_ts.tv_nsec = ms % 1000 * 1000000;
  struct timespec abs_ts;

  do
  {
    abs_ts.tv_sec = rm_ts.tv_sec;
    abs_ts.tv_nsec = rm_ts.tv_nsec;
  } while (nanosleep(&abs_ts, &rm_ts) < 0);

}

//------------------------------------------------------------------------------
// TableInfo constructor
//------------------------------------------------------------------------------
TableInfo::TableInfo(Log* logger, const BRM::TxnID txnID, const string& processName, OID tableOID,
                     const string& tableName, bool bKeepRbMetaFile)
 : fTableId(-1)
 , fBufferSize(0)
 , fFileBufSize(0)
 , fStatusTI(WriteEngine::NEW)
 , fReadBufCount(0)
 , fNumberOfColumns(0)
 , fHandle(NULL)
 , fCurrentReadBuffer(0)
 , fTotalReadRows(0)
 , fTotalErrRows(0)
 , fMaxErrorRows(5)
 , fLastBufferId(-1)
 , fFileBuffer(NULL)
 , fCurrentParseBuffer(0)
 , fNumberOfColsParsed(0)
 , fLocker(-1)
 , fTableName(tableName)
 , fTableOID(tableOID)
 , fJobId(0)
 , fLog(logger)
 , fTxnID(txnID)
 , fRBMetaWriter(processName, logger)
 , fProcessName(processName)
 , fKeepRbMetaFile(bKeepRbMetaFile)
 , fbTruncationAsError(false)
 , fImportDataMode(IMPORT_DATA_TEXT)
 , fTimeZone(dataconvert::systemTimeZoneOffset())
 , fTableLocked(false)
 , fReadFromStdin(false)
 , fReadFromS3(false)
 , fNullStringMode(false)
 , fEnclosedByChar('\0')
 , fEscapeChar('\\')
 , fProcessingBegun(false)
 , fBulkMode(BULK_MODE_LOCAL)
 , fBRMReporter(logger, tableName)
 , fTableLockID(0)
 , fRejectDataCnt(0)
 , fRejectErrCnt(0)
 , fExtentStrAlloc(tableOID, logger)
 , fOamCachePtr(oam::OamCache::makeOamCache())
 , fParquetReader(NULL)
{
  fBuffers.clear();
  fColumns.clear();
  fStartTime.tv_sec = 0;
  fStartTime.tv_usec = 0;
  string teleServerHost(config::Config::makeConfig()->getConfig("QueryTele", "Host"));

  if (!teleServerHost.empty())
  {
    int teleServerPort =
        config::Config::fromText(config::Config::makeConfig()->getConfig("QueryTele", "Port"));

    if (teleServerPort > 0)
    {
      fQtc.serverParms(QueryTeleServerParms(teleServerHost, teleServerPort));
    }
  }
}

//------------------------------------------------------------------------------
// TableInfo destructor
//------------------------------------------------------------------------------
TableInfo::~TableInfo()
{
  fBRMReporter.sendErrMsgToFile(fBRMRptFileName);
  freeProcessingBuffers();
}

//------------------------------------------------------------------------------
// Frees up processing buffer memory.  We don't reset fReadBufCount to 0,
// because BulkLoad::lockColumnForParse() is calling getNumberOfBuffers()
// and dividing by the return value.  So best not to risk returning 0.
// Once we get far enough to call this freeProcessingBuffers() function,
// the application code obviously better be completely through accessing
// fBuffers and fColumns.
//------------------------------------------------------------------------------
void TableInfo::freeProcessingBuffers()
{
  // fLog->logMsg(
  //    string("Releasing TableInfo Buffer for ")+fTableName,
  //    MSGLVL_INFO1);
  fBuffers.clear();
  fColumns.clear();
  fNumberOfColumns = 0;
}

//------------------------------------------------------------------------------
// Close any database column or dictionary store files left open for this table.
// Under "normal" circumstances, there should be no files left open when we
// reach the end of the job, but in some error cases, the parsing threads may
// bail out without closing a file.  So this function is called as part of
// EOJ cleanup for any tables that are still holding a table lock.
//
// Files will automatically get closed when the program terminates, but when
// we are preparing for a bulk rollback, we want to explicitly close the files
// before we "reopen" them and start rolling back the contents of the files.
//
// For mode1 and mode2 imports, cpimport.bin does not lock the table or perform
// a bulk rollback, and closeOpenDbFile() is not called.  We instead rely on
// the program to implicitly close the files.
//------------------------------------------------------------------------------
void TableInfo::closeOpenDbFiles()
{
  ostringstream oss;
  oss << "Closing DB files for table " << fTableName << ", left open by abnormal termination.";
  fLog->logMsg(oss.str(), MSGLVL_INFO2);

  for (unsigned int k = 0; k < fColumns.size(); k++)
  {
    stringstream oss1;
    oss1 << "Closing DB column file for: " << fColumns[k].column.colName << " (OID-"
         << fColumns[k].column.mapOid << ")";
    fLog->logMsg(oss1.str(), MSGLVL_INFO2);
    fColumns[k].closeColumnFile(false, true);

    if (fColumns[k].column.colType == COL_TYPE_DICT)
    {
      stringstream oss2;
      oss2 << "Closing DB store  file for: " << fColumns[k].column.colName << " (OID-"
           << fColumns[k].column.dctnry.dctnryOid << ")";
      fLog->logMsg(oss2.str(), MSGLVL_INFO2);
      fColumns[k].closeDctnryStore(true);
    }
  }
}

//------------------------------------------------------------------------------
// Locks this table for reading to the specified thread (locker) "if" the table
// has not yet been assigned to a read thread.
//------------------------------------------------------------------------------
bool TableInfo::lockForRead(const int& locker)
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);

  if (fLocker == -1)
  {
    if (fStatusTI == WriteEngine::NEW)
    {
      fLocker = locker;
      return true;
    }
  }

  return false;
}


// int TableInfo::parseParquetCol(std::shared_ptr<arrow::RecordBatch> batch, unsigned int k, int bs)
// {
//   int rc = NO_ERROR;
//   ColumnBufferSection* section = 0;
//   uint32_t nRowsParsed;
//   RID lastInputRowInExtent;
//   ColumnInfo& columnInfo = fColumns[k];
//   RETURN_ON_ERROR(columnInfo.fColBufferMgr->reserveSection(10 * k, bs, nRowsParsed,
//                   &section, lastInputRowInExtent));
  

//   if (nRowsParsed > 0)
//   {
//     unsigned char* buf = new unsigned char[bs * columnInfo.column.width];

//     std::shared_ptr<arrow::Array> columnData = batch->column(k);
    
//   }
//   return rc;
// }


int TableInfo::parseParquetDict(std::shared_ptr<arrow::RecordBatch> batch, unsigned int k, unsigned int cbs, int64_t bs, int batchProcessed)
{
  int rc = NO_ERROR;
  ColumnInfo& columnInfo = fColumns[k];

  ColumnBufferSection* section = 0;
  RID lastInputRowInExtent = 0;
  uint32_t nRowsParsed;
  RETURN_ON_ERROR(columnInfo.fColBufferMgr->reserveSection(bs * batchProcessed, cbs, nRowsParsed, &section, lastInputRowInExtent));

  if (nRowsParsed > 0)
  {
    char* tokenBuf = new char[nRowsParsed * 8];
    std::shared_ptr<arrow::Array> columnData = batch->column(k);
    rc = columnInfo.updateDctnryStoreParquet(columnData, nRowsParsed, tokenBuf);

    if (rc == NO_ERROR)
    {
      section->write(tokenBuf, nRowsParsed);
      delete[] tokenBuf;

      RETURN_ON_ERROR(columnInfo.fColBufferMgr->releaseSection(section));
    }
    else
    {
      delete[] tokenBuf;
    }
  }
  return rc;
}

int TableInfo::parseParquetCol(std::shared_ptr<arrow::RecordBatch> batch, unsigned int k, unsigned int cbs, int64_t bs, int batchProcessed)
{
  int rc = NO_ERROR;
  ColumnBufferSection* section = 0;
  uint32_t nRowsParsed;
  RID lastInputRowInExtent;
  ColumnInfo& columnInfo = fColumns[k];
  RETURN_ON_ERROR(columnInfo.fColBufferMgr->reserveSection(bs * batchProcessed, cbs, nRowsParsed,
                  &section, lastInputRowInExtent));
  uint64_t fAutoIncNextValue = 0;
  int64_t nullCount = batch->column(k)->null_count();
  if (nRowsParsed > 0)
  {
    if ((columnInfo.column.autoIncFlag) && (nullCount > 0))
    {
      rc = columnInfo.reserveAutoIncNums(nullCount, fAutoIncNextValue);
    }


    unsigned char* buf = new unsigned char[cbs * columnInfo.column.width];

    BLBufferStats bufStats(columnInfo.column.dataType);
    bool updateCPInfoPendingFlag = false;
    std::shared_ptr<arrow::Array> columnData = batch->column(k);
    // get current column data type
    // arrow::Type::type colType = columnData->type()->id();
    // only consider `int` type now 

    // parquetConvert(std::shared_ptr<arrow::Array>, JobColumn&, BLBufferStats&, unsigned char*)

    parquetConvert(columnData, columnInfo.column, bufStats, buf, cbs, fAutoIncNextValue);

    updateCPInfoPendingFlag = true;

    if (columnInfo.column.width <= 8)
    {
      columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.minBufferVal, bufStats.maxBufferVal,
                              columnInfo.column.dataType, columnInfo.column.width);
    }
    else
    {
      columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.bigMinBufferVal, bufStats.bigMaxBufferVal,
                              columnInfo.column.dataType, columnInfo.column.width);
    }

    // what's this rowsPerExtent for?
    lastInputRowInExtent += columnInfo.rowsPerExtent();

    if (isUnsigned(columnInfo.column.dataType))
    {
      if (columnInfo.column.width <= 8)
      {
        bufStats.minBufferVal = static_cast<int64_t>(MAX_UBIGINT);
        bufStats.maxBufferVal = static_cast<int64_t>(MIN_UBIGINT);
      }
      else
      {
        bufStats.bigMinBufferVal = -1;
        bufStats.bigMaxBufferVal = 0;
      }
      updateCPInfoPendingFlag = false;
    }
    else
    {
      if (columnInfo.column.width <= 8)
      {
        bufStats.minBufferVal = MAX_BIGINT;
        bufStats.maxBufferVal = MIN_BIGINT;
      }
      else
      {
        utils::int128Max(bufStats.bigMinBufferVal);
        utils::int128Min(bufStats.bigMaxBufferVal);
      }
      updateCPInfoPendingFlag = false;
    }

    if (updateCPInfoPendingFlag)
    {
      if (columnInfo.column.width <= 8)
      {
        columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.minBufferVal, bufStats.maxBufferVal,
                                columnInfo.column.dataType, columnInfo.column.width);
      }
      else
      {
        columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.bigMinBufferVal, bufStats.bigMaxBufferVal,
                                columnInfo.column.dataType, columnInfo.column.width);
      }
    }

    if (bufStats.satCount)
    {
      columnInfo.incSaturatedCnt(bufStats.satCount);
    }

    section->write(buf, cbs);
    delete[] buf;

    RETURN_ON_ERROR(columnInfo.fColBufferMgr->releaseSection(section));
  }
  return rc;
}

void TableInfo::parquetConvert(std::shared_ptr<arrow::Array> columnData, const JobColumn& column, BLBufferStats& bufStats, unsigned char* buf, unsigned int cbs, uint64_t& fAutoIncNextValue)
{
  char biVal;
  int iVal;
  float fVal;
  double dVal;
  short siVal;
  void* pVal;
  int32_t iDate;
  long long llVal = 0, llDate = 0;
  int128_t bigllVal = 0;
  uint64_t tmp64;
  uint32_t tmp32;
  uint8_t ubiVal;
  uint16_t usiVal;
  uint32_t uiVal;
  uint64_t ullVal;

  int width = column.width;
  switch (column.weType)
  {
    case WriteEngine::WR_FLOAT:
    {
      const float* dataPtr = columnData->data()->GetValues<float>(1);
      for (uint32_t i = 0; i < cbs; i++)
      {
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (column.fWithDefault)
          {
            fVal = column.fDefaultDbl;
            pVal = &fVal;
          }
          else
          {
            tmp32 = joblist::FLOATNULL;
            pVal = &tmp32;
            memcpy(p, pVal, width);
            continue;
          }
        }
        else
        {
          float minFltSat = column.fMinDblSat;
          float maxFltSat = column.fMaxDblSat;
          memcpy(&fVal, dataPtr + i, width);
          if (fVal > maxFltSat)
          {
            fVal = maxFltSat;
            bufStats.satCount++;
          }
          else if (fVal < minFltSat)
          {
            fVal = minFltSat;
            bufStats.satCount++;
          }
          pVal = &fVal;
        }
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_DOUBLE:
    {
      const double* dataPtr = columnData->data()->GetValues<double>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (column.fWithDefault)
          {
            dVal = column.fDefaultDbl;
            pVal = &dVal;
          }
          else
          {
            tmp64 = joblist::DOUBLENULL;
            pVal = &tmp64;
            memcpy(p, pVal, width);
            continue;
          }
        }
        else
        {
          memcpy(&dVal, dataPtr + i, width);
        }
        if (dVal > column.fMaxDblSat)
        {
          dVal = column.fMaxDblSat;
          bufStats.satCount++;
        }
        else if (dVal < column.fMinDblSat)
        {
          dVal = column.fMinDblSat;
          bufStats.satCount++;
        }
        pVal = &dVal;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_CHAR:
    {
      auto binaryArray = std::static_pointer_cast<arrow::BinaryArray>(columnData);
      int tokenLen;
      for (unsigned int i = 0; i < cbs; i++)
      {
        char charTmpBuf[MAX_COLUMN_BOUNDARY + 1] = {0};
        void* p = buf + width * i;
        if (columnData->IsNull(i))
        {
          if (column.fWithDefault)
          {
            int defLen = column.fDefaultChr.size();
            const char* defData = column.fDefaultChr.c_str();
            if (defLen > column.definedWidth)
              memcpy(charTmpBuf, defData, column.definedWidth);
            else
              memcpy(charTmpBuf, defData, defLen);
          }
          else
          {
            idbassert(width <= 8);
            for (int j = 0; j < width - 1; j++)
            {
              charTmpBuf[j] = '\377';
            }
            charTmpBuf[width - 1] = '\376';
            pVal = charTmpBuf;
            memcpy(p, pVal, width);
            continue;
            // break;
          }
        }
        else
        {
          const uint8_t* data = binaryArray->GetValue(i, &tokenLen);
          const char* dataPtr = reinterpret_cast<const char*>(data);
          if (tokenLen > column.definedWidth)
          {
            uint8_t truncate_point = utf8::utf8_truncate_point(dataPtr, column.definedWidth);
            memcpy(charTmpBuf, dataPtr, column.definedWidth - truncate_point);
            bufStats.satCount++;
          }
          else
          {
            memcpy(charTmpBuf, dataPtr, tokenLen);
          }
        }

        uint64_t compChar = uint64ToStr(*(reinterpret_cast<uint64_t*>(charTmpBuf)));
        int64_t binChar = static_cast<int64_t>(compChar);

        // Update min/max range
        uint64_t minVal = static_cast<uint64_t>(bufStats.minBufferVal);
        uint64_t maxVal = static_cast<uint64_t>(bufStats.maxBufferVal);

        if (compChar < minVal)
          bufStats.minBufferVal = binChar;
        if (compChar > maxVal)
          bufStats.maxBufferVal = binChar;

        pVal = charTmpBuf;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_SHORT:
    {
      long long origVal;
      // use char type here
      const short* dataPtr = columnData->data()->GetValues<short>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              origVal = column.fDefaultInt;
            }
            else
            {
              siVal = joblist::SMALLINTNULL;
              pVal = &siVal;
              memcpy(p, pVal, width);
              // to jump to next loop
              continue;
            }
          }
          else
          {
            // FIXME: no fAutoIncNextValue in tableInfo
            // fill 1 temporarily
            // origVal = tAutoIncNextValue++;
            origVal = fAutoIncNextValue++;
          }
        }
        else
        {
          // origVal = *(dataPtr + i);
          if ((column.dataType == CalpontSystemCatalog::DECIMAL) ||
              (column.dataType == CalpontSystemCatalog::UDECIMAL))
          {
            const int128_t* dataPtr1 = reinterpret_cast<const int128_t*>(dataPtr);
            // auto dataPtr1 = std::static_pointer_cast<int128_t>(dataPtr);
            origVal = *(dataPtr1 + i);
          }
          else
          {
            origVal = *(dataPtr + i);
          }
          // memcpy(&siVal, dataPtr + i, width);
          // origVal = siVal;
        }

        if (origVal < column.fMinIntSat)
        {
          origVal = column.fMinIntSat;
          bSatVal = true;
        }
        else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(column.fMaxIntSat);
          bSatVal = true;
        }

        if (bSatVal)
          bufStats.satCount++;

        if (origVal < bufStats.minBufferVal)
          bufStats.minBufferVal = origVal;
        if (origVal > bufStats.maxBufferVal)
          bufStats.maxBufferVal = origVal;
        
        siVal = origVal;
        pVal = &siVal;
        memcpy(p, pVal, width);
        
      }
      break;
    }


    case WriteEngine::WR_USHORT:
    {
      int64_t origVal = 0;
      const uint16_t* dataPtr = columnData->data()->GetValues<uint16_t>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              origVal = static_cast<int64_t>(column.fDefaultUInt);
            }
            else
            {
              usiVal = joblist::USMALLINTNULL;
              pVal = &usiVal;
              memcpy(p, pVal, width);
              // FIXME:
              // to jump to next loop
              continue;
            }
          }
          else
          {
            // FIXME: no fAutoIncNextValue in tableInfo
            // fill 1 temporarily
            // origVal = tAutoIncNextValue++;
            origVal = fAutoIncNextValue++;
          }
        }
        else
        {
          origVal = *(dataPtr + i);
          // memcpy(&usiVal, dataPtr + i, width);
          // origVal = usiVal;
        }

        if (origVal < column.fMinIntSat)
        {
          origVal = column.fMinIntSat;
          bSatVal = true;
        }
        else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(column.fMaxIntSat);
          bSatVal = true;
        }

        if (bSatVal)
          bufStats.satCount++;

        uint64_t uVal = origVal;

        if (uVal < static_cast<uint64_t>(bufStats.minBufferVal))
          bufStats.minBufferVal = origVal;
        if (uVal > static_cast<uint64_t>(bufStats.maxBufferVal))
          bufStats.maxBufferVal = origVal;
        
        usiVal = origVal;
        pVal = &usiVal;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_BYTE:
    {
      // TODO:support boolean

      long long origVal;
      // FIXME:if use int8_t here, it will take 8 bool value of parquet array
      // if (columnData->type_id() == arrow::Type::type::BOOL)
      // {
      std::shared_ptr<arrow::BooleanArray> boolArray = std::static_pointer_cast<arrow::BooleanArray>(columnData);
      // }
      const int8_t* dataPtr = columnData->data()->GetValues<int8_t>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              origVal = column.fDefaultInt;
            }
            else
            {
              biVal = joblist::TINYINTNULL;
              pVal = &biVal;
              memcpy(p, pVal, width);
              continue;
            }

          }
          else
          {
            origVal = fAutoIncNextValue++;
          }
        }
        else
        {
          // memcpy(&biVal, dataPtr + i, width);
          // origVal = *(dataPtr + i);
          if ((column.dataType == CalpontSystemCatalog::DECIMAL) ||
              (column.dataType == CalpontSystemCatalog::UDECIMAL))
          {
            const int128_t* dataPtr1 = reinterpret_cast<const int128_t*>(dataPtr);
            // auto dataPtr1 = std::static_pointer_cast<int128_t>(dataPtr);
            origVal = *(dataPtr1 + i);
          }
          else if (columnData->type_id() == arrow::Type::type::BOOL)
          {
            origVal = boolArray->Value(i);
          }
          else
          {
            origVal = *(dataPtr + i);
          }
        }

        if (origVal < column.fMinIntSat)
        {
          origVal = column.fMinIntSat;
        }
        else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(column.fMaxIntSat);
          bSatVal = true;
        }

        if (bSatVal)
          bufStats.satCount++;


        if (origVal < bufStats.minBufferVal)
          bufStats.minBufferVal = origVal;
        
        if (origVal > bufStats.maxBufferVal)
          bufStats.maxBufferVal = origVal;

        biVal = origVal;
        pVal = &biVal;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_UBYTE:
    {
      int64_t origVal = 0;
      const uint8_t* dataPtr = columnData->data()->GetValues<uint8_t>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              origVal = static_cast<int64_t>(column.fDefaultUInt);
            }
            else
            {
              ubiVal = joblist::UTINYINTNULL;
              pVal = &ubiVal;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            origVal = fAutoIncNextValue++;
          }
        }
        else
        {
          // memcpy(&ubiVal, dataPtr + i, width);
          origVal = *(dataPtr + i);
        }

        if (origVal < column.fMinIntSat)
        {
          origVal = column.fMinIntSat;
          bSatVal = true;
        }
        else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(column.fMaxIntSat);
          bSatVal = true;
        }

        if (bSatVal)
          bufStats.satCount++;

        uint64_t uVal = origVal;

        if (uVal < static_cast<uint64_t>(bufStats.minBufferVal))
          bufStats.minBufferVal = origVal;
        
        if (uVal > static_cast<uint64_t>(bufStats.maxBufferVal))
          bufStats.maxBufferVal = origVal;

        ubiVal = origVal;
        pVal = &ubiVal;
        memcpy(p, pVal, width);
      }
      break;
    }
    
    case WriteEngine::WR_LONGLONG:
    {
      if (column.dataType != CalpontSystemCatalog::DATETIME &&
          column.dataType != CalpontSystemCatalog::TIMESTAMP &&
          column.dataType != CalpontSystemCatalog::TIME)
      {
        const long long *dataPtr = columnData->data()->GetValues<long long>(1);
        for (unsigned int i = 0; i < cbs; i++)
        {
          void *p = buf + i * width;
          bool bSatVal = false;
          if (columnData->IsNull(i))
          {
            if (!column.autoIncFlag)
            {
              if (column.fWithDefault)
              {
                llVal = column.fDefaultInt;
              }
              else
              {
                llVal = joblist::BIGINTNULL;
                pVal = &llVal;
                memcpy(p, pVal, width);
                continue;
              }
            }
            else
            {
              llVal = fAutoIncNextValue++;
            }
          }
          else
          {
            // memcpy(&llVal, dataPtr + i, width);
            if ((column.dataType == CalpontSystemCatalog::DECIMAL) ||
                (column.dataType == CalpontSystemCatalog::UDECIMAL))
            {
              const int128_t* dataPtr1 = reinterpret_cast<const int128_t*>(dataPtr);
              llVal = *(dataPtr1 + i);
              // long double ldVal = static_cast<long double>(llVal);
              // for (int ii = 0; ii < column.scale; ii++)
              // {
              //   ldVal *= 10;
              // }
              // if (ldVal > LLONG_MAX)
              // {
              //   bSatVal = true;
              //   llVal = LLONG_MAX;
              // }
              // else if (ldVal < LLONG_MIN)
              // {
              //   bSatVal = true;
              //   llVal = LLONG_MIN;
              // }
              // else
              // {
              //   llVal = ldVal;
              // }
            }
            else
            {
              llVal = *(dataPtr + i);
            }
          }

          if (llVal < column.fMinIntSat)
          {
            llVal = column.fMinIntSat;
            bSatVal = true;
          }
          else if (llVal > static_cast<int64_t>(column.fMaxIntSat))
          {
            llVal = static_cast<int64_t>(column.fMaxIntSat);
            bSatVal = true;
          }

          if (bSatVal)
            bufStats.satCount++;

          // Update min/max range
          if (llVal < bufStats.minBufferVal)
            bufStats.minBufferVal = llVal;

          if (llVal > bufStats.maxBufferVal)
            bufStats.maxBufferVal = llVal;

          pVal = &llVal;
          memcpy(p, pVal, width);
        }
      }
      else if (column.dataType == CalpontSystemCatalog::TIME)
      {
        // time conversion here
        // for parquet, there are two time type, time32 and time64
        // if it's time32, unit is millisecond, int32
        if (columnData->type_id() == arrow::Type::type::TIME32 || columnData->type_id() == arrow::Type::type::NA)
        {
          std::shared_ptr<arrow::Time32Array> timeArray = std::static_pointer_cast<arrow::Time32Array>(columnData);
          for (unsigned int i = 0; i < cbs; i++)
          {
            // bool bSatVal = false;
            void *p = buf + i * width;
            if (columnData->IsNull(i))
            {
              if (column.fWithDefault)
              {
                llDate = column.fDefaultInt;
              }
              else
              {
                llDate = joblist::TIMENULL;
                pVal = &llDate;
                memcpy(p, pVal, width);
                continue;
              }
            }
            else
            {
              // timeVal is millisecond since midnight
              int32_t timeVal = timeArray->Value(i);
              llDate = dataconvert::DataConvert::convertArrowColumnTime32(timeVal);

            }
            if (llDate < bufStats.minBufferVal)
              bufStats.minBufferVal = llDate;
            if (llDate > bufStats.maxBufferVal)
              bufStats.maxBufferVal = llDate;
            pVal = &llDate;
            memcpy(p, pVal, width);
          }
        }
        // if it's time64, unit is microsecond, int64
        else if (columnData->type_id() == arrow::Type::type::TIME64)
        {
          std::shared_ptr<arrow::Time64Array> timeArray = std::static_pointer_cast<arrow::Time64Array>(columnData);
          for (unsigned int i = 0; i < cbs; i++)
          {
            // bool bSatVal = false;
            void *p = buf + i * width;
            if (columnData->IsNull(i))
            {
              if (column.fWithDefault)
              {
                llDate = column.fDefaultInt;
              }
              else
              {
                llDate = joblist::TIMENULL;
                pVal = &llDate;
                memcpy(p, pVal, width);
                continue;
              }
            }
            else
            {
              // timeVal is macrosecond since midnight
              int64_t timeVal = timeArray->Value(i);
              llDate = dataconvert::DataConvert::convertArrowColumnTime64(timeVal);

            }
            if (llDate < bufStats.minBufferVal)
              bufStats.minBufferVal = llDate;
            if (llDate > bufStats.maxBufferVal)
              bufStats.maxBufferVal = llDate;
            pVal = &llDate;
            memcpy(p, pVal, width);
          }
        }
      }
      else if (column.dataType == CalpontSystemCatalog::TIMESTAMP)
      {
        // timestamp conversion here
        // default column type is TIMESTAMP
        // default unit is millisecond
        std::shared_ptr<arrow::TimestampArray> timeArray = std::static_pointer_cast<arrow::TimestampArray>(columnData);
        for (unsigned int i = 0; i < cbs; i++)
        {
          // bool bSatVal = false;
          void *p = buf + i * width;
          if (columnData->IsNull(i))
          {
            if (column.fWithDefault)
            {
              llDate = column.fDefaultInt;
            }
            else
            {
              llDate = joblist::TIMESTAMPNULL;
              pVal = &llDate;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            int64_t timeVal = timeArray->Value(i);
            llDate = timeVal;
          }
          if (llDate < bufStats.minBufferVal)
            bufStats.minBufferVal = llDate;
          if (llDate > bufStats.maxBufferVal)
            bufStats.maxBufferVal = llDate;
          pVal = &llDate;
          memcpy(p, pVal, width);
        }
      }
      else
      {
        // datetime conversion here
        // default column type is TIMESTAMP
        std::shared_ptr<arrow::TimestampArray> timeArray = std::static_pointer_cast<arrow::TimestampArray>(columnData);
        for (unsigned int i = 0; i < cbs; i++)
        {
          // bool bSatVal = false;
          int rc = 0;
          void *p = buf + i * width;
          if (columnData->IsNull(i))
          {
            if (column.fWithDefault)
            {
              llDate = column.fDefaultInt;
            }
            else
            {
              llDate = joblist::DATETIMENULL;
              pVal = &llDate;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            // int64_t timestampVal = timeArray->Value(i);
            // TODO:To get the datetime info of timestampVal
            int64_t timeVal = timeArray->Value(i);
            llDate = dataconvert::DataConvert::convertArrowColumnDatetime(timeVal, rc);
            // continue;
          }
          if (rc == 0)
          {
            if (llDate < bufStats.minBufferVal)
              bufStats.minBufferVal = llDate;

            if (llDate > bufStats.maxBufferVal)
              bufStats.maxBufferVal = llDate;
          }
          else
          {
            llDate = 0;
            bufStats.satCount++;
          }
          pVal = &llDate;
          memcpy(p, pVal, width);
        }

      }
      break;
    }

    case WriteEngine::WR_BINARY:
    {
      // Parquet does not have data type with 128 byte
      // const int128_t* dataPtr = static_pointer_cast<int128_t>(columnData);
      // const int128_t* dataPtr = columnData->data()->GetValues<int128_t>(1);
      std::shared_ptr<arrow::Decimal128Array> decimalArray = std::static_pointer_cast<arrow::Decimal128Array>(columnData);
      std::shared_ptr<arrow::DecimalType> fType = std::static_pointer_cast<arrow::DecimalType>(decimalArray->type());
      // int32_t fPrecision = fType->precision();
      // int32_t fScale = fType->scale();
      const int128_t* dataPtr = decimalArray->data()->GetValues<int128_t>(1);


      for (unsigned int i = 0; i < cbs; i++)
      {
        void* p = buf + i * width;
        bool bSatVal = false;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              bigllVal = column.fDefaultWideDecimal;
            }
            else
            {
              bigllVal = datatypes::Decimal128Null;
              pVal = &bigllVal;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            bigllVal = fAutoIncNextValue++;
          }
        }
        else
        {
          // TODO:
          // compare parquet data precision and scale with table column precision and scale
          
          // Get int and frac part
          memcpy(&bigllVal, dataPtr + i, sizeof(int128_t));
          // dataconvert::parquet_int_value(bigllVal, column.scale, column.precision, fScale, fPrecision, &bSatVal);


        }
        if (bSatVal)
          bufStats.satCount++;

        //TODO: no bSatVal change here
        if (bigllVal < bufStats.bigMinBufferVal)
          bufStats.bigMinBufferVal = bigllVal;
        
        if (bigllVal > bufStats.bigMaxBufferVal)
          bufStats.bigMaxBufferVal = bigllVal;
        
        pVal = &bigllVal;
        memcpy(p, pVal, width);
      }
      break;
    }


    case WriteEngine::WR_ULONGLONG:
    {
      const uint64_t* dataPtr = columnData->data()->GetValues<uint64_t>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        // const uint64_t* dataPtr = static_pointer_cast<uint64_t>(columnData);
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              ullVal = column.fDefaultUInt;
            }
            else
            {
              ullVal = joblist::UBIGINTNULL;
              pVal = &ullVal;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            ullVal = fAutoIncNextValue++;
          }
        }
        else
        {
          memcpy(&ullVal, dataPtr+i, width);
        }
        if (ullVal > column.fMaxIntSat)
        {
          ullVal = column.fMaxIntSat;
          bSatVal = true;
        }
        // TODO:why no comparsion with column.fMinIntSat
        

        if (bSatVal)
          bufStats.satCount++;
        if (ullVal < static_cast<uint64_t>(bufStats.minBufferVal))
          bufStats.minBufferVal = static_cast<int64_t>(ullVal);

        if (ullVal > static_cast<uint64_t>(bufStats.maxBufferVal))
          bufStats.maxBufferVal = static_cast<int64_t>(ullVal);

        pVal = &ullVal;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_UMEDINT:
    case WriteEngine::WR_UINT:
    {
      int64_t origVal;
      // const uint32_t* dataPtr = static_pointer_cast<uint32_t>(columnData);
      const uint32_t* dataPtr = columnData->data()->GetValues<uint32_t>(1);
      for (unsigned int i = 0; i < cbs; i++)
      {
        bool bSatVal = false;
        void* p = buf + i * width;
        if (columnData->IsNull(i))
        {
          if (!column.autoIncFlag)
          {
            if (column.fWithDefault)
            {
              origVal = static_cast<int64_t>(column.fDefaultUInt);
            }
            else
            {
              uiVal = joblist::UINTNULL;
              pVal = &uiVal;
              memcpy(p, pVal, width);
              // TODO:continue jump to next loop?
              continue;
            }
          }
          else
          {
            origVal = fAutoIncNextValue++;
          }
        }
        else
        {
          // memcpy(&uiVal, dataPtr + i, width);
          origVal = *(dataPtr + i);
        }
        if (origVal < column.fMinIntSat)
        {
          origVal = column.fMinIntSat;
          bSatVal = true;
        }
        else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(column.fMaxIntSat);
          bSatVal = true;
        }

        if (bSatVal)
          bufStats.satCount++;

        // Update min/max range
        uint64_t uVal = origVal;

        if (uVal < static_cast<uint64_t>(bufStats.minBufferVal))
          bufStats.minBufferVal = origVal;

        if (uVal > static_cast<uint64_t>(bufStats.maxBufferVal))
          bufStats.maxBufferVal = origVal;

        uiVal = origVal;
        pVal = &uiVal;
        memcpy(p, pVal, width);
      }
      break;
    }

    case WriteEngine::WR_MEDINT:
    case WriteEngine::WR_INT:
    default:
    {
      if (column.dataType != CalpontSystemCatalog::DATE)
      {
        const int* dataPtr = columnData->data()->GetValues<int>(1);
        for (unsigned int i = 0; i < cbs; i++)
        {
          bool bSatVal = false;
          void* p = buf + i * width;
          long long origVal;
          if (columnData->IsNull(i))
          {
            if (!column.autoIncFlag)
            {
              if (column.fWithDefault)
              {
                origVal = column.fDefaultInt;
              }
              else
              {
                iVal = joblist::INTNULL;
                pVal = &iVal;
                memcpy(p, pVal, width);
                continue;
              }
            }
            else
            {
              origVal = fAutoIncNextValue++;
            }
          }
          else
          {
            // memcpy(&iVal, dataPtr + i, width);
            // origVal = (long long)iVal;
            // memcpy(&origVal, )
            if ((column.dataType == CalpontSystemCatalog::DECIMAL) ||
                (column.dataType == CalpontSystemCatalog::UDECIMAL))
            {
              const int128_t* dataPtr1 = reinterpret_cast<const int128_t*>(dataPtr);
              // auto dataPtr1 = std::static_pointer_cast<int128_t>(dataPtr);
              origVal = *(dataPtr1 + i);
              // long double ldVal = static_cast<long double>(origVal);
              // for (int ii = 0; ii< column.scale; ii++)
              // {
              //   ldVal *= 10;
              // }
              // if (ldVal > LLONG_MAX)
              // {
              //   bSatVal = true;
              //   origVal = LLONG_MAX;
              // }
              // else if (ldVal < LLONG_MIN)
              // {
              //   bSatVal = true;
              //   origVal = LLONG_MIN;
              // }
              // else
              // {
              //   origVal = ldVal;
              // }
            }
            else
            {
              origVal = *(dataPtr + i);
            }
          }

          if (origVal < column.fMinIntSat)
          {
            origVal = column.fMinIntSat;
            bSatVal = true;
          }
          else if (origVal > static_cast<int64_t>(column.fMaxIntSat))
          {
            origVal = static_cast<int64_t>(column.fMaxIntSat);
            bSatVal = true;
          }
          if (bSatVal)
            bufStats.satCount++;

          if (origVal < bufStats.minBufferVal)
            bufStats.minBufferVal = origVal;

          if (origVal > bufStats.maxBufferVal)
            bufStats.maxBufferVal = origVal;

          iVal = (int)origVal;
          pVal = &iVal;
          memcpy(p, pVal, width);
        }
      }
      else
      {
        // date conversion here
        std::shared_ptr<arrow::Date32Array> timeArray = std::static_pointer_cast<arrow::Date32Array>(columnData);
        for (unsigned int i = 0; i < cbs; i++)
        {
          int rc = 0;
          void* p = buf + i * width;
          if (columnData->IsNull(i))
          {
            if (column.fWithDefault)
            {
              iDate = column.fDefaultInt;
            }
            else
            {
              iDate = joblist::DATENULL;
              pVal = &iDate;
              memcpy(p, pVal, width);
              continue;
            }
          }
          else
          {
            int32_t dayVal = timeArray->Value(i);
            iDate = dataconvert::DataConvert::ConvertArrowColumnDate(dayVal, rc);
          }
          if (rc == 0)
          {
            if (iDate < bufStats.minBufferVal)
              bufStats.minBufferVal = iDate;

            if (iDate > bufStats.maxBufferVal)
              bufStats.maxBufferVal = iDate;
          }
          else
          {
            iDate = 0;
            bufStats.satCount++;
          }
          pVal = &iDate;
          memcpy(p, pVal, width);
        }
      }
    }
  }
}

int TableInfo::readParquetData()
{
  int rc = NO_ERROR;
  int fileCounter = 0;
  // read first file temporarily
  fFileName = fLoadFileList[fileCounter];

  //---------------------------------------------------
  std::cout << "Reading by RecordBatchReader" << std::endl;

  arrow::MemoryPool* pool = arrow::default_memory_pool();

  // Configure general Parquet reader settings
  auto reader_properties = parquet::ReaderProperties(pool);
  reader_properties.set_buffer_size(4096 * 4);
  reader_properties.enable_buffered_stream();

  // Configure Arrow-specific Parquet reader settings
  auto arrow_reader_props = parquet::ArrowReaderProperties();
  // TODO:batch_size is set as a parameter
  int64_t bs = 10;
  arrow_reader_props.set_batch_size(bs);  // default 64 * 1024

  parquet::arrow::FileReaderBuilder reader_builder;
  PARQUET_THROW_NOT_OK(
      reader_builder.OpenFile(fFileName, /*memory_map=*/false, reader_properties));
  reader_builder.memory_pool(pool);
  reader_builder.properties(arrow_reader_props);

  std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
  PARQUET_ASSIGN_OR_THROW(arrow_reader, reader_builder.Build());

  std::shared_ptr<::arrow::RecordBatchReader> rb_reader;
  PARQUET_THROW_NOT_OK(arrow_reader->GetRecordBatchReader(&rb_reader));

  int batch_processed = 0;
  for (arrow::Result<std::shared_ptr<arrow::RecordBatch>> maybe_batch : *rb_reader)
  {
    // Operate on each batch...
    PARQUET_ASSIGN_OR_THROW(auto batch, maybe_batch);
    unsigned int current_batch_size = batch->num_rows();
    // for every column in batch, parse it into ColumnBuffer
    
    // fNumberOfColumns-1 because there is a special column named aux which is internal column
    // And later `aux` should be processed individually

    for (unsigned k = 0; k < fNumberOfColumns-1; k++)
    {
      // parseParquet(batch, k, current_batch_size);
      if (fColumns[k].column.colType == COL_TYPE_DICT)
      {
        rc = parseParquetDict(batch, k, current_batch_size, bs, batch_processed);
        //TODO: if input data is spanned to 2 extents.
        continue;
      }
      else
      {
        rc = parseParquetCol(batch, k ,current_batch_size, bs, batch_processed);
          // rc = parseParquetCol(batch, k, bs);
        // int rc = NO_ERROR;

      }
    }
    // process `aux` column
    ColumnInfo& columnInfo = fColumns[fNumberOfColumns-1];
    ColumnBufferSection* section = 0;
    uint32_t nRowsParsed;
    RID lastInputRowInExtent;
    // ColumnInfo& columnInfo = fColumns[k];
    RETURN_ON_ERROR(columnInfo.fColBufferMgr->reserveSection(bs * batch_processed, current_batch_size, nRowsParsed,
                    &section, lastInputRowInExtent));
    if (nRowsParsed > 0)
    {
      unsigned char* buf = new unsigned char[current_batch_size * columnInfo.column.width];

      BLBufferStats bufStats(columnInfo.column.dataType);
      bool updateCPInfoPendingFlag = false;

      // std::shared_ptr<arrow::Array> columnData = batch->column(k);
      // get current column data type
      // arrow::Type::type colType = columnData->type()->id();
      // only consider `int` type now 
      // const char* data_ptr = columnData->data()->GetValues<char>(1);
      for (uint32_t i = 0; i < current_batch_size; i++)
      {
        unsigned char* p = buf + i * columnInfo.column.width;
        void *t;
        int64_t origVal;
        // int32_t iVal;
        bool bSatVal = false;
        origVal = static_cast<int64_t>(columnInfo.column.fDefaultUInt);
        // Saturate the value
        if (origVal < columnInfo.column.fMinIntSat)
        {
          origVal = columnInfo.column.fMinIntSat;
          bSatVal = true;
        }
        else if (origVal > static_cast<int64_t>(columnInfo.column.fMaxIntSat))
        {
          origVal = static_cast<int64_t>(columnInfo.column.fMaxIntSat);
          bSatVal = true;
        }
        if (bSatVal)
          bufStats.satCount++;
        uint64_t uVal = origVal;

        if (uVal < static_cast<uint64_t>(bufStats.minBufferVal))
          bufStats.minBufferVal = origVal;

        if (uVal > static_cast<uint64_t>(bufStats.maxBufferVal))
          bufStats.maxBufferVal = origVal;
        
        uint8_t ubiVal = origVal;
        t = &ubiVal;
        memcpy(p, t, 1);

        updateCPInfoPendingFlag = true;

        if ((RID)(bs * batch_processed + i) == lastInputRowInExtent)
        {
          if (columnInfo.column.width <= 8)
          {
            columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.minBufferVal, bufStats.maxBufferVal,
                                    columnInfo.column.dataType, columnInfo.column.width);
          }
          else
          {
            columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.bigMinBufferVal, bufStats.bigMaxBufferVal,
                                    columnInfo.column.dataType, columnInfo.column.width);
          }

          // what's this rowsPerExtent for?
          lastInputRowInExtent += columnInfo.rowsPerExtent();

          if (isUnsigned(columnInfo.column.dataType))
          {
            if (columnInfo.column.width <= 8)
            {
              bufStats.minBufferVal = static_cast<int64_t>(MAX_UBIGINT);
              bufStats.maxBufferVal = static_cast<int64_t>(MIN_UBIGINT);
            }
            else
            {
              bufStats.bigMinBufferVal = -1;
              bufStats.bigMaxBufferVal = 0;
            }
            updateCPInfoPendingFlag = false;
          }
          else
          {
            if (columnInfo.column.width <= 8)
            {
              bufStats.minBufferVal = MAX_BIGINT;
              bufStats.maxBufferVal = MIN_BIGINT;
            }
            else
            {
              utils::int128Max(bufStats.bigMinBufferVal);
              utils::int128Min(bufStats.bigMaxBufferVal);
            }
            updateCPInfoPendingFlag = false;
          }
        }


      }

      if (updateCPInfoPendingFlag)
      {
        if (columnInfo.column.width <= 8)
        {
          columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.minBufferVal, bufStats.maxBufferVal,
                                  columnInfo.column.dataType, columnInfo.column.width);
        }
        else
        {
          columnInfo.updateCPInfo(lastInputRowInExtent, bufStats.bigMinBufferVal, bufStats.bigMaxBufferVal,
                                  columnInfo.column.dataType, columnInfo.column.width);
        }
      }

      if (bufStats.satCount)
      {
        columnInfo.incSaturatedCnt(bufStats.satCount);
      }

      section->write(buf, current_batch_size);
      delete[] buf;

      RETURN_ON_ERROR(columnInfo.fColBufferMgr->releaseSection(section));

    }




    batch_processed++;
  }

  // TODO:setParseComplete
  // After all the data has been parsed
  // Accumulate list of HWM dictionary blocks to be flushed from cache
  for (unsigned k = 0; k < fNumberOfColumns; k++)
  {
    std::vector<BRM::LBID_t> dictBlksToFlush;
    fColumns[k].getDictFlushBlks(dictBlksToFlush);

    for (unsigned kk = 0; kk < dictBlksToFlush.size(); kk++)
    {
      fDictFlushBlks.push_back(dictBlksToFlush[kk]);
    }

    int rc = fColumns[k].finishParsing();
    if (rc != NO_ERROR)
    {
      return rc;
    }
  }

  if (!idbdatafile::IDBPolicy::useHdfs())
  {
    if (fDictFlushBlks.size() > 0)
    {
      cacheutils::flushPrimProcAllverBlocks(fDictFlushBlks);
      fDictFlushBlks.clear();
    }
  }
  
  rc = synchronizeAutoInc();
  if (rc != NO_ERROR)
  {
    return rc;
  }

  std::vector<DBRootExtentInfo> segFileInfo;
  for (unsigned i = 0; i < fColumns.size(); i++)
  {
    DBRootExtentInfo extentInfo;
    fColumns[i].getSegFileInfo(extentInfo);
    segFileInfo.push_back(extentInfo);
  }


  rc = validateColumnHWMs(0, segFileInfo, "Ending");

  if (rc != NO_ERROR)
  {
    return rc;
  }

  rc = confirmDBFileChanges();

  if (rc != NO_ERROR)
  {
    return rc;
  }

  rc = finishBRM();
  if (rc != NO_ERROR)
  {
    return rc;
  }

  rc = changeTableLockState();

  if (rc != NO_ERROR)
  {
    return rc;
  }

  deleteTempDBFileChanges();
  deleteMetaDataRollbackFile();

  rc = releaseTableLock();

  if (rc != NO_ERROR)
  {
    return rc;
  }

  fStatusTI = WriteEngine::PARSE_COMPLETE;

  freeProcessingBuffers();

  return rc;
}


//------------------------------------------------------------------------------
// Loop thru reading the import file(s) assigned to this TableInfo object.
//------------------------------------------------------------------------------
int TableInfo::readTableData()
{
  RID validTotalRows = 0;
  RID totalRowsPerInputFile = 0;
  int64_t totalRowsParquet = 0;
  int filesTBProcessed = fLoadFileList.size();
  int fileCounter = 0;
  unsigned long long qtSentAt = 0;
  if (fImportDataMode != IMPORT_DATA_PARQUET)
  {
    if (fHandle == NULL)
    {
      fFileName = fLoadFileList[fileCounter];
      int rc = openTableFile();

      if (rc != NO_ERROR)
      {
        // Mark the table status as error and exit.
        boost::mutex::scoped_lock lock(fSyncUpdatesTI);
        fStatusTI = WriteEngine::ERR;
        return rc;
      }

      fileCounter++;
    }

  }
  else
  {
    if (fParquetReader == NULL)
    {
      fFileName = fLoadFileList[fileCounter];
      int rc = openTableFileParquet(totalRowsParquet);
      fileCounter++;
      if (rc != NO_ERROR)
      {
        // Mark the table status as error and exit.
        boost::mutex::scoped_lock lock(fSyncUpdatesTI);
        fStatusTI = WriteEngine::ERR;
        return rc;
      }
    }

  }

  timeval readStart;
  gettimeofday(&readStart, NULL);
  ostringstream ossStartMsg;
  ossStartMsg << "Start reading and loading table " << fTableName;
  fLog->logMsg(ossStartMsg.str(), MSGLVL_INFO2);
  fProcessingBegun = true;

  ImportTeleStats its;
  its.job_uuid = fJobUUID;
  its.import_uuid = QueryTeleClient::genUUID();
  its.msg_type = ImportTeleStats::IT_START;
  its.start_time = QueryTeleClient::timeNowms();
  its.table_list.push_back(fTableName);
  its.rows_so_far.push_back(0);
  its.system_name = fOamCachePtr->getSystemName();
  its.module_name = fOamCachePtr->getModuleName();
  string tn = getTableName();
  its.schema_name = string(tn, 0, tn.find('.'));
  fQtc.postImportTele(its);

  //
  // LOOP to read all the import data for this table
  //
  while (true)
  {
    // See if JobStatus has been set to terminate by another thread
    if (BulkStatus::getJobStatus() == EXIT_FAILURE)
    {
      boost::mutex::scoped_lock lock(fSyncUpdatesTI);
      fStartTime = readStart;
      fStatusTI = WriteEngine::ERR;
      its.msg_type = ImportTeleStats::IT_TERM;
      its.rows_so_far.pop_back();
      its.rows_so_far.push_back(0);
      fQtc.postImportTele(its);
      throw SecondaryShutdownException(
          "TableInfo::"
          "readTableData(1) responding to job termination");
    }

// @bug 3271: Conditionally compile the thread deadlock debug logging
#ifdef DEADLOCK_DEBUG
    // @bug2099+.  Temp hack to diagnose deadlock.
    struct timeval tvStart;
    gettimeofday(&tvStart, 0);
    bool report = false;
    bool reported = false;
    // @bug2099-
#else
    const bool report = false;
#endif

#ifdef PROFILE
    Stats::startReadEvent(WE_STATS_WAIT_FOR_READ_BUF);
#endif

    //
    // LOOP to wait for, and read, the next avail BulkLoadBuffer object
    //
    while (!isBufferAvailable(report))
    {
      // See if JobStatus has been set to terminate by another thread
      if (BulkStatus::getJobStatus() == EXIT_FAILURE)
      {
        boost::mutex::scoped_lock lock(fSyncUpdatesTI);
        fStartTime = readStart;
        fStatusTI = WriteEngine::ERR;
        its.msg_type = ImportTeleStats::IT_TERM;
        its.rows_so_far.pop_back();
        its.rows_so_far.push_back(0);
        fQtc.postImportTele(its);
        throw SecondaryShutdownException(
            "TableInfo::"
            "readTableData(2) responding to job termination");
      }

      // Sleep and check the condition again.
      sleepMS(1);
#ifdef DEADLOCK_DEBUG

      // @bug2099+
      if (report)
        report = false;  // report one time.

      if (!reported)
      {
        struct timeval tvNow;
        gettimeofday(&tvNow, 0);

        if ((tvNow.tv_sec - tvStart.tv_sec) > 100)
        {
          time_t t = time(0);
          char timeString[50];
          ctime_r(&t, timeString);
          timeString[strlen(timeString) - 1] = '\0';
          ostringstream oss;
          oss << endl
              << timeString << ": "
              << "TableInfo::readTableData: " << fTableName << "; Diff is " << (tvNow.tv_sec - tvStart.tv_sec)
              << endl;
          cout << oss.str();
          cout.flush();
          report = true;
          reported = true;
        }
      }

      // @bug2099-
#endif
    }

#ifdef PROFILE
    Stats::stopReadEvent(WE_STATS_WAIT_FOR_READ_BUF);
    Stats::startReadEvent(WE_STATS_READ_INTO_BUF);
#endif

    int readBufNo = fCurrentReadBuffer;
    int prevReadBuf = (fCurrentReadBuffer - 1);

    if (prevReadBuf < 0)
      prevReadBuf = fReadBufCount + prevReadBuf;

    // We keep a running total of read errors;  fMaxErrorRows specifies
    // the error limit.  Here's where we see how many more errors we
    // still have below the limit, and we pass this to fillFromFile().
    unsigned allowedErrCntThisCall = ((fMaxErrorRows > fTotalErrRows) ? (fMaxErrorRows - fTotalErrRows) : 0);

    // Fill in the specified buffer.
    // fTotalReadRowsPerInputFile is ongoing total number of rows read,
    //   per input file.
    // validTotalRows is ongoing total of valid rows read for all files
    //   pertaining to this DB table.
    int readRc;
    if (fReadFromS3)
    {
      readRc = fBuffers[readBufNo].fillFromMemory(fBuffers[prevReadBuf], fFileBuffer, fS3ReadLength,
                                                  &fS3ParseLength, totalRowsPerInputFile, validTotalRows,
                                                  fColumns, allowedErrCntThisCall);
    }
    else
    {
      if (fImportDataMode != IMPORT_DATA_PARQUET)
      {
        readRc = fBuffers[readBufNo].fillFromFile(fBuffers[prevReadBuf], fHandle, totalRowsPerInputFile,
                                                  validTotalRows, fColumns, allowedErrCntThisCall);
      }
      else
      {
        readRc = fBuffers[readBufNo].fillFromFileParquet(totalRowsPerInputFile, validTotalRows);
      }
    }

    if (readRc != NO_ERROR)
    {
      // error occurred.
      // need to exit.
      // mark the table status as error and exit.
      {
        boost::mutex::scoped_lock lock(fSyncUpdatesTI);
        fStartTime = readStart;
        fStatusTI = WriteEngine::ERR;
        fBuffers[readBufNo].setStatusBLB(WriteEngine::ERR);
      }
      closeTableFile();

      // Error occurred on next row not read, so increment
      // totalRowsPerInputFile row count for the error msg
      WErrorCodes ec;
      ostringstream oss;
      oss << "Error reading import file " << fFileName << "; near line " << totalRowsPerInputFile + 1 << "; "
          << ec.errorString(readRc);
      fLog->logMsg(oss.str(), readRc, MSGLVL_ERROR);

      its.msg_type = ImportTeleStats::IT_TERM;
      its.rows_so_far.pop_back();
      its.rows_so_far.push_back(0);
      fQtc.postImportTele(its);

      return readRc;
    }

#ifdef PROFILE
    Stats::stopReadEvent(WE_STATS_READ_INTO_BUF);
#endif
    its.msg_type = ImportTeleStats::IT_PROGRESS;
    its.rows_so_far.pop_back();
    its.rows_so_far.push_back(totalRowsPerInputFile);
    unsigned long long thisRows = static_cast<unsigned long long>(totalRowsPerInputFile);
    thisRows /= 1000000;

    if (thisRows > qtSentAt)
    {
      fQtc.postImportTele(its);
      qtSentAt = thisRows;
    }

    // Check if there were any errors in the read data.
    // if yes, copy it to the error list.
    // if the number of errors is greater than the maximum error count
    // mark the table status as error and exit.
    // call the method to copy the errors
    writeErrorList(&fBuffers[readBufNo].getErrorRows(), &fBuffers[readBufNo].getExactErrorRows(), false);
    fBuffers[readBufNo].clearErrRows();

    if (fTotalErrRows > fMaxErrorRows)
    {
      // flush the reject data file and output the rejected rows
      // flush err file and output the rejected row id and the reason.
      writeErrorList(0, 0, true);

      // number of errors > maximum allowed. hence return error.
      {
        boost::mutex::scoped_lock lock(fSyncUpdatesTI);
        fStartTime = readStart;
        fStatusTI = WriteEngine::ERR;
        fBuffers[readBufNo].setStatusBLB(WriteEngine::ERR);
      }
      closeTableFile();
      ostringstream oss5;
      oss5 << "Actual error row count(" << fTotalErrRows << ") exceeds the max error rows(" << fMaxErrorRows
           << ") allowed for table " << fTableName;
      fLog->logMsg(oss5.str(), ERR_BULK_MAX_ERR_NUM, MSGLVL_ERROR);

      // List Err and Bad files to report file (if applicable)
      fBRMReporter.rptMaxErrJob(fBRMRptFileName, fErrFiles, fBadFiles);

      its.msg_type = ImportTeleStats::IT_TERM;
      its.rows_so_far.pop_back();
      its.rows_so_far.push_back(0);
      fQtc.postImportTele(its);

      return ERR_BULK_MAX_ERR_NUM;
    }

    // mark the buffer status as read complete.
    {
#ifdef PROFILE
      Stats::startReadEvent(WE_STATS_WAIT_TO_COMPLETE_READ);
#endif
      boost::mutex::scoped_lock lock(fSyncUpdatesTI);
#ifdef PROFILE
      Stats::stopReadEvent(WE_STATS_WAIT_TO_COMPLETE_READ);
      Stats::startReadEvent(WE_STATS_COMPLETING_READ);
#endif

      fStartTime = readStart;
      fBuffers[readBufNo].setStatusBLB(WriteEngine::READ_COMPLETE);

      fCurrentReadBuffer = (fCurrentReadBuffer + 1) % fReadBufCount;

      // bufferCount++;
      if ((fHandle && feof(fHandle)) || (fReadFromS3 && (fS3ReadLength == fS3ParseLength)) || (totalRowsPerInputFile == (RID)totalRowsParquet))
      {
        timeval readFinished;
        gettimeofday(&readFinished, NULL);

        closeTableFile();

        if (fReadFromStdin)
        {
          fLog->logMsg("Finished loading " + fTableName + " from STDIN" + ", Time taken = " +
                           Convertor::int2Str((int)(readFinished.tv_sec - readStart.tv_sec)) + " seconds",
                       //" seconds; bufferCount-"+Convertor::int2Str(bufferCount),
                       MSGLVL_INFO2);
        }
        else if (fReadFromS3)
        {
          fLog->logMsg("Finished loading " + fTableName + " from S3" + ", Time taken = " +
                           Convertor::int2Str((int)(readFinished.tv_sec - readStart.tv_sec)) + " seconds",
                       //" seconds; bufferCount-"+Convertor::int2Str(bufferCount),
                       MSGLVL_INFO2);
        }
        else
        {
          fLog->logMsg("Finished reading file " + fFileName + ", Time taken = " +
                           Convertor::int2Str((int)(readFinished.tv_sec - readStart.tv_sec)) + " seconds",
                       //" seconds; bufferCount-"+Convertor::int2Str(bufferCount),
                       MSGLVL_INFO2);
        }

        // flush the reject data file and output the rejected rows
        // flush err file and output the rejected row id and the reason.
        writeErrorList(0, 0, true);

        // If > 1 file for this table, then open next file in the list
        if (fileCounter < filesTBProcessed)
        {
          fFileName = fLoadFileList[fileCounter];
          int rc;
          if (fImportDataMode != IMPORT_DATA_PARQUET)
          {
            rc = openTableFile();
          }
          else
          {
            rc = openTableFileParquet(totalRowsParquet);
          }

          if (rc != NO_ERROR)
          {
            // Mark the table status as error and exit.
            fStatusTI = WriteEngine::ERR;
            return rc;
          }

          fileCounter++;
          fTotalReadRows += totalRowsPerInputFile;
          totalRowsPerInputFile = 0;
        }
        else  // All files read for this table; break out of read loop
        {
          fStatusTI = WriteEngine::READ_COMPLETE;
          fLastBufferId = readBufNo;
          fTotalReadRows += totalRowsPerInputFile;
          break;
        }

        gettimeofday(&readStart, NULL);
      }  // reached EOF

#ifdef PROFILE
      Stats::stopReadEvent(WE_STATS_COMPLETING_READ);
#endif
    }  // mark buffer status as read-complete within scope of a mutex
  }    // loop to read all data for this table

  its.msg_type = ImportTeleStats::IT_SUMMARY;
  its.end_time = QueryTeleClient::timeNowms();
  its.rows_so_far.pop_back();
  its.rows_so_far.push_back(fTotalReadRows);
  fQtc.postImportTele(its);
  fQtc.waitForQueues();

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// writeErrorList()
//   errorRows    - vector of row numbers and corresponding error messages
//   errorDatRows - vector of bad rows that have been rejected
//
// Adds errors pertaining to a specific buffer, to the cumulative list of
// errors to be reported to the user.
//------------------------------------------------------------------------------
void TableInfo::writeErrorList(const std::vector<std::pair<RID, std::string> >* errorRows,
                               const std::vector<std::string>* errorDatRows, bool bCloseFile)
{
  size_t errorRowsCount = 0;
  size_t errorDatRowsCount = 0;

  if (errorRows)
    errorRowsCount = errorRows->size();

  if (errorDatRows)
    errorDatRowsCount = errorDatRows->size();

  if ((errorRowsCount > 0) || (errorDatRowsCount > 0) || (bCloseFile))
  {
    boost::mutex::scoped_lock lock(fErrorRptInfoMutex);

    if ((errorRowsCount > 0) || (bCloseFile))
      writeErrReason(errorRows, bCloseFile);

    if ((errorDatRowsCount > 0) || (bCloseFile))
      writeBadRows(errorDatRows, bCloseFile);

    fTotalErrRows += errorRowsCount;
  }
}

//------------------------------------------------------------------------------
// Parse the specified column (columnId) in the specified buffer (bufferId).
//------------------------------------------------------------------------------
int TableInfo::parseColumn(const int& columnId, const int& bufferId, double& processingTime)
{
  // parse the column
  // note the time and update the column's last processing time
  timeval parseStart, parseEnd;
  gettimeofday(&parseStart, NULL);

  // Will need to check whether the column needs to extend.
  // If size of the file is less than the required size, extend the column
  int rc = fBuffers[bufferId].parse(fColumns[columnId]);
  gettimeofday(&parseEnd, NULL);

  processingTime = (parseEnd.tv_usec / 1000 + parseEnd.tv_sec * 1000) -
                   (parseStart.tv_usec / 1000 + parseStart.tv_sec * 1000);

  return rc;
}

// int TableInfo::parseColumnParquet(const int& columnId, double& processingTime)
// {
//   int rc = NO_ERROR;
//   timeval parseStart, parseEnd;
//   gettimeofday(&parseStart, NULL);


//   return rc;
// }

//------------------------------------------------------------------------------
// Mark the specified column (columnId) in the specified buffer (bufferId) as
// PARSE_COMPLETE.  If this is the last column to be parsed for this buffer,
// then mark the buffer as PARSE_COMPLETE.
// If the last buffer for this table has been read (fLastBufferId != -1), then
// see if all the data for columnId has been parsed for all the buffers, in
// which case we are finished parsing columnId.
// If this is the last column to finish parsing for this table, then mark the
// table status as PARSE_COMPLETE.
//------------------------------------------------------------------------------
int TableInfo::setParseComplete(const int& columnId, const int& bufferId, double processingTime)
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);

  // Check table status in case race condition results in this function
  // being called after fStatusTI was set to ERR by another thread.
  if (fStatusTI == WriteEngine::ERR)
    return ERR_UNKNOWN;

  fColumns[columnId].lastProcessingTime = processingTime;
#ifdef PROFILE
  fColumns[columnId].totalProcessingTime += processingTime;
#endif

  // Set buffer status to complete if setColumnStatus indicates that
  // all the columns are complete
  if (fBuffers[bufferId].setColumnStatus(columnId, WriteEngine::PARSE_COMPLETE))
    fBuffers[bufferId].setStatusBLB(WriteEngine::PARSE_COMPLETE);

  // fLastBufferId != -1 means the Read thread has read the last
  // buffer for this table
  if (fLastBufferId != -1)
  {
    // check if the status of the column in all the fBuffers is parse
    // complete then update the column status as parse complete.
    bool allBuffersDoneForAColumn = true;

    for (int i = 0; i < fReadBufCount; ++i)
    {
      // check the status of the column in this buffer.
      Status bufferStatus = fBuffers[i].getStatusBLB();

      if ((bufferStatus == WriteEngine::READ_COMPLETE) || (bufferStatus == WriteEngine::PARSE_COMPLETE))
      {
        if (fBuffers[i].getColumnStatus(columnId) != WriteEngine::PARSE_COMPLETE)
        {
          allBuffersDoneForAColumn = false;
          break;
        }
      }
    }

    // allBuffersDoneForAColumn==TRUE means we are finished parsing columnId
    if (allBuffersDoneForAColumn)
    {
      // Accumulate list of HWM dictionary blocks to be flushed from cache
      std::vector<BRM::LBID_t> dictBlksToFlush;
      fColumns[columnId].getDictFlushBlks(dictBlksToFlush);

      for (unsigned kk = 0; kk < dictBlksToFlush.size(); kk++)
      {
        fDictFlushBlks.push_back(dictBlksToFlush[kk]);
      }

      int rc = fColumns[columnId].finishParsing();

      if (rc != NO_ERROR)
      {
        WErrorCodes ec;
        ostringstream oss;
        oss << "setParseComplete completion error; "
               "Failed to load table: "
            << fTableName << "; " << ec.errorString(rc);
        fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
        fStatusTI = WriteEngine::ERR;
        return rc;
      }

      fNumberOfColsParsed++;

      //
      // If all columns have been parsed, then finished with this tbl
      //
      if (fNumberOfColsParsed >= fNumberOfColumns)
      {
        // After closing the column and dictionary store files,
        // flush any updated dictionary blocks in PrimProc.
        // We only do this for non-HDFS.  For HDFS we don't want
        // to flush till "after" we have "confirmed" all the file
        // changes, which flushes the changes to disk.
        if (!idbdatafile::IDBPolicy::useHdfs())
        {
          if (fDictFlushBlks.size() > 0)
          {
#ifdef PROFILE
            Stats::startParseEvent(WE_STATS_FLUSH_PRIMPROC_BLOCKS);
#endif
            if (fLog->isDebug(DEBUG_2))
            {
              ostringstream oss;
              oss << "Dictionary cache flush: ";
              for (uint32_t i = 0; i < fDictFlushBlks.size(); i++)
              {
                oss << fDictFlushBlks[i] << ", ";
              }
              oss << endl;
              fLog->logMsg(oss.str(), MSGLVL_INFO1);
            }
            cacheutils::flushPrimProcAllverBlocks(fDictFlushBlks);
#ifdef PROFILE
            Stats::stopParseEvent(WE_STATS_FLUSH_PRIMPROC_BLOCKS);
#endif
            fDictFlushBlks.clear();
          }
        }

        // Update auto-increment next value if applicable.
        rc = synchronizeAutoInc();

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: autoInc update error; "
                 "Failed to load table: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;
          return rc;
        }

        //..Validate that all the HWM's are consistent and in-sync
        std::vector<DBRootExtentInfo> segFileInfo;

        for (unsigned i = 0; i < fColumns.size(); ++i)
        {
          DBRootExtentInfo extentInfo;
          fColumns[i].getSegFileInfo(extentInfo);
          segFileInfo.push_back(extentInfo);
        }

        rc = validateColumnHWMs(0, segFileInfo, "Ending");

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: HWM validation error; "
                 "Failed to load table: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;

          ostringstream oss2;
          oss2 << "Ending HWMs for table " << fTableName << ": ";

          for (unsigned int n = 0; n < fColumns.size(); n++)
          {
            oss2 << std::endl;
            oss2 << "  " << fColumns[n].column.colName << "; DBRoot/part/seg/hwm: " << segFileInfo[n].fDbRoot
                 << "/" << segFileInfo[n].fPartition << "/" << segFileInfo[n].fSegment << "/"
                 << segFileInfo[n].fLocalHwm;
          }

          fLog->logMsg(oss2.str(), MSGLVL_INFO1);

          return rc;
        }

        //..Confirm changes to DB files (necessary for HDFS)
        rc = confirmDBFileChanges();

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: Error confirming DB changes; "
                 "Failed to load table: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;
          return rc;
        }

        //..Update BRM with HWM and Casual Partition info, etc.
        rc = finishBRM();

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: BRM error; "
                 "Failed to load table: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;
          return rc;
        }

        // Change table lock state to CLEANUP
        rc = changeTableLockState();

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: table lock state change error; "
                 "Table load completed: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;
          return rc;
        }

        // Finished with this table, so delete bulk rollback
        // meta data file and release the table lock.
        deleteTempDBFileChanges();
        deleteMetaDataRollbackFile();

        rc = releaseTableLock();

        if (rc != NO_ERROR)
        {
          WErrorCodes ec;
          ostringstream oss;
          oss << "setParseComplete: table lock release error; "
                 "Failed to load table: "
              << fTableName << "; " << ec.errorString(rc);
          fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
          fStatusTI = WriteEngine::ERR;
          return rc;
        }

#ifdef PROFILE

        // Loop through columns again to print out the elapsed
        // parse times
        for (unsigned i = 0; i < fColumns.size(); ++i)
        {
          ostringstream ossColTime;
          ossColTime << "Column " << i << "; OID-" << fColumns[i].column.mapOid << "; parseTime-"
                     << (fColumns[i].totalProcessingTime / 1000.0) << " seconds";
          fLog->logMsg(ossColTime.str(), MSGLVL_INFO1);
        }

#endif

        timeval endTime;
        gettimeofday(&endTime, 0);
        double elapsedTime = (endTime.tv_sec + (endTime.tv_usec / 1000000.0)) -
                             (fStartTime.tv_sec + (fStartTime.tv_usec / 1000000.0));

        fStatusTI = WriteEngine::PARSE_COMPLETE;
        reportTotals(elapsedTime);

        // Reduce memory use by allocating and releasing as needed
        freeProcessingBuffers();

      }  // end of if (fNumberOfColsParsed >= fNumberOfColumns)
    }    // end of if (allBuffersDoneForAColumn)
  }      // end of if (fLastBufferId != -1)

  // If we finished parsing the buffer associated with currentParseBuffer,
  // but have not finshed the entire table, then advance currentParseBuffer.
  if ((fStatusTI != WriteEngine::PARSE_COMPLETE) &&
      (fBuffers[bufferId].getStatusBLB() == WriteEngine::PARSE_COMPLETE))
  {
    // Find the BulkLoadBuffer object that is next in line to be parsed
    // and assign fCurrentParseBuffer accordingly.  Break out of the
    // loop if we wrap all the way around and catch up with the current-
    // Read buffer.
    if (bufferId == fCurrentParseBuffer)
    {
      int currentParseBuffer = fCurrentParseBuffer;

      while (fBuffers[currentParseBuffer].getStatusBLB() == WriteEngine::PARSE_COMPLETE)
      {
        currentParseBuffer = (currentParseBuffer + 1) % fReadBufCount;
        fCurrentParseBuffer = currentParseBuffer;

        if (fCurrentParseBuffer == fCurrentReadBuffer)
          break;
      }
    }
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Report summary totals to applicable destination (stdout, cpimport.bin log
// file, BRMReport file (for mode1) etc).
// elapsedTime is number of seconds taken to import this table.
//------------------------------------------------------------------------------
void TableInfo::reportTotals(double elapsedTime)
{
  ostringstream oss1;
  oss1 << "For table " << fTableName << ": " << fTotalReadRows << " rows processed and "
       << (fTotalReadRows - fTotalErrRows) << " rows inserted.";

  fLog->logMsg(oss1.str(), MSGLVL_INFO1);

  ostringstream oss2;
  oss2 << "For table " << fTableName << ": "
       << "Elapsed time to load this table: " << elapsedTime << " secs";

  fLog->logMsg(oss2.str(), MSGLVL_INFO2);

  // @bug 3504: Loop through columns to print saturation counts
  std::vector<boost::tuple<execplan::CalpontSystemCatalog::ColDataType, uint64_t, uint64_t> > satCounts;

  for (unsigned i = 0; i < fColumns.size(); ++i)
  {
    // std::string colName(fTableName);
    // colName += '.';
    // colName += fColumns[i].column.colName;
    long long satCount = fColumns[i].saturatedCnt();

    satCounts.push_back(boost::make_tuple(fColumns[i].column.dataType, fColumns[i].column.mapOid, satCount));

    if (satCount > 0)
    {
      // @bug 3375: report invalid dates/times set to null
      ostringstream ossSatCnt;
      ossSatCnt << "Column " << fTableName << '.' << fColumns[i].column.colName << "; Number of ";

      if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::DATE)
      {
        ossSatCnt << "invalid dates replaced with zero value : ";
      }
      else if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::DATETIME)
      {
        // bug5383
        ossSatCnt << "invalid date/times replaced with zero value : ";
      }

      else if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::TIMESTAMP)
      {
        ossSatCnt << "invalid timestamps replaced with zero value : ";
      }
      else if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::TIME)
      {
        ossSatCnt << "invalid times replaced with zero value : ";
      }
      else if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::CHAR)
        ossSatCnt << "character strings truncated: ";
      else if (fColumns[i].column.dataType == execplan::CalpontSystemCatalog::VARCHAR)
        ossSatCnt << "character strings truncated: ";
      else
        ossSatCnt << "rows inserted with saturated values: ";

      ossSatCnt << satCount;
      fLog->logMsg(ossSatCnt.str(), MSGLVL_WARNING);
    }
  }

  logging::Message::Args tblFinishedMsgArgs;
  tblFinishedMsgArgs.add(fJobId);
  tblFinishedMsgArgs.add(fTableName);
  tblFinishedMsgArgs.add((fTotalReadRows - fTotalErrRows));
  SimpleSysLog::instance()->logMsg(tblFinishedMsgArgs, logging::LOG_TYPE_INFO, logging::M0083);

  // Bug1375 - cpimport.bin did not add entries to the transaction
  //          log file: data_mods.log
  if ((fTotalReadRows - fTotalErrRows) > 0)
    logToDataMods(fjobFileName, oss1.str());

  // Log totals in report file if applicable
  fBRMReporter.reportTotals(fTotalReadRows, (fTotalReadRows - fTotalErrRows), satCounts);
}

//------------------------------------------------------------------------------
// Report BRM updates to a report file or to BRM directly.
//------------------------------------------------------------------------------
int TableInfo::finishBRM()
{
  // Collect the CP and HWM information for all the columns
  for (unsigned i = 0; i < fColumns.size(); ++i)
  {
    fColumns[i].getBRMUpdateInfo(fBRMReporter);
  }

  // We use mutex not to synchronize contention among parallel threads,
  // because we should be the only thread accessing the fErrFiles and
  // fBadFiles at this point.  But we do use the mutex as a memory barrier
  // to make sure we have the latest copy of the data.
  std::vector<std::string>* errFiles = 0;
  std::vector<std::string>* badFiles = 0;
  {
    boost::mutex::scoped_lock lock(fErrorRptInfoMutex);
    errFiles = &fErrFiles;
    badFiles = &fBadFiles;
  }

  // Save the info just collected, to a report file or send to BRM
  int rc = fBRMReporter.sendBRMInfo(fBRMRptFileName, *errFiles, *badFiles);

  return rc;
}

//------------------------------------------------------------------------------
// Update status of table to reflect an error.
// No need to update the buffer or column status, because we are not going to
// continue the job anyway.  Other threads should terminate when they see that
// the JobStatus has been set to EXIT_FAILURE and/or the table status has been
// set to WriteEngine::ERR.
//------------------------------------------------------------------------------
void TableInfo::setParseError()
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);
  fStatusTI = WriteEngine::ERR;
}

//------------------------------------------------------------------------------
// Locks a column from the specified buffer (bufferId) for the specified parse
// thread (id); and returns the column id.  A return value of -1 means no
// column could be locked for parsing.
//------------------------------------------------------------------------------
// @bug2099. Temporary hack to diagnose deadlock.
// Added report parm and couts below.
int TableInfo::getColumnForParse(const int& id, const int& bufferId, bool report)
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);
  double maxTime = 0;
  int columnId = -1;

  while (true)
  {
    // See if JobStatus has been set to terminate by another thread
    if (BulkStatus::getJobStatus() == EXIT_FAILURE)
    {
      fStatusTI = WriteEngine::ERR;
      throw SecondaryShutdownException(
          "TableInfo::"
          "getColumnForParse() responding to job termination");
    }

    if (!bufferReadyForParse(bufferId, report))
      return -1;

    // @bug2099+
    ostringstream oss;

    if (report)
    {
      oss << " ----- " << pthread_self() << ":fBuffers[" << bufferId <<
          "]: (colLocker,status,lasttime)- ";
    }

    // @bug2099-

    for (unsigned k = 0; k < fNumberOfColumns; ++k)
    {
      // @bug2099+
      if (report)
      {
        Status colStatus = fBuffers[bufferId].getColumnStatus(k);
        int colLocker = fBuffers[bufferId].getColumnLocker(k);

        string colStatusStr;
        ColumnInfo::convertStatusToString(colStatus, colStatusStr);

        oss << '(' << colLocker << ',' << colStatusStr << ',' << fColumns[k].lastProcessingTime << ") ";
      }

      // @bug2099-

      if (fBuffers[bufferId].getColumnLocker(k) == -1)
      {
        if (columnId == -1)
          columnId = k;
        else if (fColumns[k].lastProcessingTime == 0)
        {
          if (fColumns[k].column.width >= fColumns[columnId].column.width)
            columnId = k;
        }
        else if (fColumns[k].lastProcessingTime > maxTime)
        {
          maxTime = fColumns[k].lastProcessingTime;
          columnId = k;
        }
      }
    }

    // @bug2099+
    if (report)
    {
      oss << "; selected colId: " << columnId;

      if (columnId != -1)
        oss << "; maxTime: " << maxTime;

      oss << endl;

      if (!BulkLoad::disableConsoleOutput())
      {
        cout << oss.str();
        cout.flush();
      }
    }

    // @bug2099-

    if (columnId == -1)
      return -1;

    if (fBuffers[bufferId].tryAndLockColumn(columnId, id))
    {
      return columnId;
    }
  }
}

//------------------------------------------------------------------------------
// Check if the specified buffer is ready for parsing (status == READ_COMPLETE)
// @bug 2099.  Temporary hack to diagnose deadlock.  Added report parm
//             and couts below.
//------------------------------------------------------------------------------
bool TableInfo::bufferReadyForParse(const int& bufferId, bool report) const
{
  if (fBuffers.size() == 0)
    return false;

  Status stat = fBuffers[bufferId].getStatusBLB();

  if (report)
  {
    ostringstream oss;
    string bufStatusStr;
    ColumnInfo::convertStatusToString(stat, bufStatusStr);
    oss << " --- " << pthread_self() <<
        ":fBuffers[" << bufferId << "]=" << bufStatusStr << " (" << stat << ")" << std::endl;
    cout << oss.str();
  }

  return (stat == WriteEngine::READ_COMPLETE) ? true : false;
}

//------------------------------------------------------------------------------
// Create the specified number (noOfBuffer) of BulkLoadBuffer objects and store
// them in fBuffers.  jobFieldRefList lists the fields in this import.
// fixedBinaryRecLen is fixed record length for binary imports (it is n/a
// for text bulk loads).
//------------------------------------------------------------------------------
int TableInfo::initializeBuffers(int noOfBuffers, const JobFieldRefList& jobFieldRefList,
                                 unsigned int fixedBinaryRecLen)
{

  fReadBufCount = noOfBuffers;

  // initialize and populate the buffer vector.
  for (int i = 0; i < fReadBufCount; ++i)
  {
    BulkLoadBuffer* buffer =
        new BulkLoadBuffer(fNumberOfColumns, fBufferSize, fLog, i, fTableName, jobFieldRefList);
    buffer->setColDelimiter(fColDelim);
    buffer->setNullStringMode(fNullStringMode);
    buffer->setEnclosedByChar(fEnclosedByChar);
    buffer->setEscapeChar(fEscapeChar);
    buffer->setTruncationAsError(getTruncationAsError());
    buffer->setImportDataMode(fImportDataMode, fixedBinaryRecLen);
    buffer->setTimeZone(fTimeZone);
    fBuffers.push_back(buffer);
  }
  if (!fS3Key.empty())
  {
    ms3_library_init();
    ms3 = ms3_init(fS3Key.c_str(), fS3Secret.c_str(), fS3Region.c_str(), fS3Host.c_str());
    if (!ms3)
    {
      ostringstream oss;
      oss << "Error initiating S3 library";
      fLog->logMsg(oss.str(), ERR_FILE_OPEN, MSGLVL_ERROR);
      return ERR_FILE_OPEN;
    }
  }
  return 0;
}

//------------------------------------------------------------------------------
// Add the specified ColumnInfo object (info) into this table's fColumns vector.
//------------------------------------------------------------------------------
void TableInfo::addColumn(ColumnInfo* info)
{
  fColumns.push_back(info);
  fNumberOfColumns = fColumns.size();

  fExtentStrAlloc.addColumn(info->column.mapOid, info->column.width, info->column.dataType);
}


int TableInfo::openTableFileParquet(int64_t &totalRowsParquet)
{
  if (fParquetReader != NULL)
    return NO_ERROR;
  std::shared_ptr<arrow::io::ReadableFile> infile;
  PARQUET_ASSIGN_OR_THROW(infile, arrow::io::ReadableFile::Open(fFileName, arrow::default_memory_pool()));
  PARQUET_THROW_NOT_OK(parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &fReader));
  //TODO: batch_size set here as hyperparameter
  fReader->set_batch_size(10);
  PARQUET_THROW_NOT_OK(fReader->ScanContents({0}, 10, &totalRowsParquet));
  PARQUET_THROW_NOT_OK(fReader->GetRecordBatchReader(&fParquetReader));
  // fParquetIter = fParquetReader->begin();
  // fParquetIter++;
  // initialize fBuffers batch source
  for (int i = 0; i < fReadBufCount; ++i)
  {
    fBuffers[i].setParquetReader(fParquetReader);
  }
  return NO_ERROR;

}

//------------------------------------------------------------------------------
// Open the file corresponding to fFileName so that we can import it's contents.
// A buffer is also allocated and passed to setvbuf().
// If fReadFromStdin is true, we just assign stdin to our fHandle for reading.
//------------------------------------------------------------------------------
int TableInfo::openTableFile()
{
  if (fHandle != NULL)
    return NO_ERROR;

  if (fReadFromStdin)
  {
    fHandle = stdin;

    // Not 100% sure that calling setvbuf on stdin does much, but in
    // some tests, it made a slight difference.
    fFileBuffer = new char[fFileBufSize];
    setvbuf(fHandle, fFileBuffer, _IOFBF, fFileBufSize);
    ostringstream oss;
    oss << BOLD_START << "Reading input from STDIN to import into table " << fTableName << "..." << BOLD_STOP;
    fLog->logMsg(oss.str(), MSGLVL_INFO1);
  }
  else if (fReadFromS3)
  {
    int res;
    res = ms3_get(ms3, fS3Bucket.c_str(), fFileName.c_str(), (uint8_t**)&fFileBuffer, &fS3ReadLength);
    fS3ParseLength = 0;
    if (res)
    {
      ostringstream oss;
      oss << "Error retrieving file " << fFileName << " from S3: ";
      if (ms3_server_error(ms3))
      {
        oss << ms3_server_error(ms3);
      }
      else
      {
        oss << ms3_error(res);
      }
      fLog->logMsg(oss.str(), ERR_FILE_OPEN, MSGLVL_ERROR);
      return ERR_FILE_OPEN;
    }
  }
  else
  {
    if (fImportDataMode == IMPORT_DATA_TEXT)
      fHandle = fopen(fFileName.c_str(), "r");
    else
      fHandle = fopen(fFileName.c_str(), "rb");

    if (fHandle == NULL)
    {
      int errnum = errno;
      ostringstream oss;
      oss << "Error opening import file " << fFileName << ". " << strerror(errnum);
      fLog->logMsg(oss.str(), ERR_FILE_OPEN, MSGLVL_ERROR);

      // return an error; caller should set fStatusTI if needed
      return ERR_FILE_OPEN;
    }

    // now the input load file is available for reading the data.
    // read the data from the load file into the buffers.
    fFileBuffer = new char[fFileBufSize];
    setvbuf(fHandle, fFileBuffer, _IOFBF, fFileBufSize);

    ostringstream oss;
    oss << "Opening " << fFileName << " to import into table " << fTableName;
    fLog->logMsg(oss.str(), MSGLVL_INFO2);
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Close the current open file we have been importing.
//------------------------------------------------------------------------------
void TableInfo::closeTableFile()
{
  if (fHandle)
  {
    // If reading from stdin, we don't delete the buffer out from under
    // the file handle, because stdin is still open.  This will cause a
    // memory leak, but when using stdin, we can only read in 1 table.
    // So it's not like we will be leaking multiple buffers for several
    // tables over the life of the job.
    if (!fReadFromStdin)
    {
      fclose(fHandle);
      delete[] fFileBuffer;
    }

    fHandle = 0;
  }
  else if (ms3)
  {
    ms3_free((uint8_t*)fFileBuffer);
  }
}

//------------------------------------------------------------------------------
// "Grabs" the current read buffer for TableInfo so that the read thread that
// is calling this function, can read the next buffer's set of data.
//------------------------------------------------------------------------------
// @bug2099. Temporary hack to diagnose deadlock.
// Added report parm and couts below.
bool TableInfo::isBufferAvailable(bool report)
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);
  Status bufferStatus = fBuffers[fCurrentReadBuffer].getStatusBLB();

  if ((bufferStatus == WriteEngine::PARSE_COMPLETE) || (bufferStatus == WriteEngine::NEW))
  {
    // reset buffer status and column locks while we have
    // an fSyncUpdatesTI lock
    fBuffers[fCurrentReadBuffer].setStatusBLB(WriteEngine::READ_PROGRESS);
    fBuffers[fCurrentReadBuffer].resetColumnLocks();
    return true;
  }

  if (report)
  {
    ostringstream oss;
    string bufferStatusStr;
    ColumnInfo::convertStatusToString(bufferStatus, bufferStatusStr);
    oss << "  Buffer status is " << bufferStatusStr << ". " << endl;
    oss << "  fCurrentReadBuffer is " << fCurrentReadBuffer << endl;
    cout << oss.str();
    cout.flush();
  }

  return false;
}

//------------------------------------------------------------------------------
// Report whether rows were rejected, and if so, then list them out into the
// reject file.
//------------------------------------------------------------------------------
void TableInfo::writeBadRows(const std::vector<std::string>* errorDatRows, bool bCloseFile)
{
  size_t errorDatRowsCount = 0;

  if (errorDatRows)
    errorDatRowsCount = errorDatRows->size();

  if (errorDatRowsCount > 0)
  {
    if (!fRejectDataFile.is_open())
    {
      ostringstream rejectFileName;

      if (fErrorDir.size() > 0)
      {
        rejectFileName << fErrorDir << basename(getFileName().c_str());
      }
      else
      {
        if (fReadFromS3)
        {
          rejectFileName << basename(getFileName().c_str());
        }
        else
        {
          rejectFileName << getFileName();
        }
      }

      rejectFileName << ".Job_" << fJobId << '_' << ::getpid() << BAD_FILE_SUFFIX;
      fRejectDataFileName = rejectFileName.str();
      fRejectDataFile.open(rejectFileName.str().c_str(), ofstream::out);

      if (!fRejectDataFile)
      {
        ostringstream oss;
        oss << "Unable to create file: " << rejectFileName.str() << ";  Check permission.";
        fLog->logMsg(oss.str(), ERR_FILE_OPEN, MSGLVL_ERROR);

        return;
      }
    }

    for (std::vector<string>::const_iterator iter = errorDatRows->begin(); iter != errorDatRows->end();
         ++iter)
    {
      fRejectDataFile << *iter;
    }

    fRejectDataCnt += errorDatRowsCount;
  }

  if (bCloseFile)
  {
    if (fRejectDataFile.is_open())
      fRejectDataFile.close();

    fRejectDataFile.clear();

    if (fRejectDataCnt > 0)
    {
      ostringstream oss;
      std::string rejectFileNameToLog;

      // Construct/report complete file name and save in list of files
      boost::filesystem::path p(fRejectDataFileName);

      if (!p.has_root_path())
      {
        // We could fail here having fixed size buffer
        char cwdPath[4096];
        char* buffPtr = &cwdPath[0];
        buffPtr = getcwd(cwdPath, sizeof(cwdPath));
        boost::filesystem::path rejectFileName2(buffPtr);
        rejectFileName2 /= fRejectDataFileName;
        fBadFiles.push_back(rejectFileName2.string());

        rejectFileNameToLog = rejectFileName2.string();
      }
      else
      {
        fBadFiles.push_back(fRejectDataFileName);

        rejectFileNameToLog = fRejectDataFileName;
      }

      oss << "Number of rows with bad data = " << fRejectDataCnt
          << ".  Exact rows are listed in file located here: " << fErrorDir;
      fLog->logMsg(oss.str(), MSGLVL_INFO1);

      fRejectDataCnt = 0;
    }
  }
}

//------------------------------------------------------------------------------
// Report whether rows were rejected, and if so, then list out the row numbers
// and error reasons into the error file.
//------------------------------------------------------------------------------
void TableInfo::writeErrReason(const std::vector<std::pair<RID, string> >* errorRows, bool bCloseFile)
{
  size_t errorRowsCount = 0;

  if (errorRows)
    errorRowsCount = errorRows->size();

  if (errorRowsCount > 0)
  {
    if (!fRejectErrFile.is_open())
    {
      ostringstream errFileName;

      if (fErrorDir.size() > 0)
      {
        errFileName << fErrorDir << basename(getFileName().c_str());
      }
      else
      {
        if (fReadFromS3)
        {
          errFileName << basename(getFileName().c_str());
        }
        else
        {
          errFileName << getFileName();
        }
      }

      errFileName << ".Job_" << fJobId << '_' << ::getpid() << ERR_FILE_SUFFIX;
      fRejectErrFileName = errFileName.str();
      fRejectErrFile.open(errFileName.str().c_str(), ofstream::out);

      if (!fRejectErrFile)
      {
        ostringstream oss;
        oss << "Unable to create file: " << errFileName.str() << ";  Check permission.";
        fLog->logMsg(oss.str(), ERR_FILE_OPEN, MSGLVL_ERROR);

        return;
      }
    }

    for (std::vector<std::pair<RID, std::string> >::const_iterator iter = errorRows->begin();
         iter != errorRows->end(); ++iter)
    {
      fRejectErrFile << "Line number " << iter->first << ";  Error: " << iter->second << endl;
    }

    fRejectErrCnt += errorRowsCount;
  }

  if (bCloseFile)
  {
    if (fRejectErrFile.is_open())
      fRejectErrFile.close();

    fRejectErrFile.clear();

    if (fRejectErrCnt > 0)
    {
      ostringstream oss;
      std::string errFileNameToLog;

      // Construct/report complete file name and save in list of files
      boost::filesystem::path p(fRejectErrFileName);

      if (!p.has_root_path())
      {
        char cwdPath[4096];
        char* buffPtr = &cwdPath[0];
        buffPtr = getcwd(cwdPath, sizeof(cwdPath));
        boost::filesystem::path errFileName2(buffPtr);
        errFileName2 /= fRejectErrFileName;
        fErrFiles.push_back(errFileName2.string());

        errFileNameToLog = errFileName2.string();
      }
      else
      {
        fErrFiles.push_back(fRejectErrFileName);

        errFileNameToLog = fRejectErrFileName;
      }

      oss << "Number of rows with errors = " << fRejectDataCnt
          << ".  Exact rows are listed in file located here: " << fErrorDir;
      fLog->logMsg(oss.str(), MSGLVL_INFO1);

      fRejectErrCnt = 0;
    }
  }
}

//------------------------------------------------------------------------------
// Logs "Bulkload |Job" message along with the specified message text
// (messageText) to the critical log.
//------------------------------------------------------------------------------
void TableInfo::logToDataMods(const string& jobFile, const string& messageText)
{
  logging::Message::Args args;

  unsigned subsystemId = 19;  // writeengine

  logging::LoggingID loggingId(subsystemId, 0, fTxnID.id, 0);
  logging::MessageLog messageLog(loggingId, LOG_LOCAL1);

  logging::Message m(8);
  args.add("Bulkload |Job: " + jobFile);
  args.add("|" + messageText);
  m.format(args);
  messageLog.logInfoMessage(m);
}

//------------------------------------------------------------------------------
// Acquires DB table lock for this TableInfo object.
// Function employs retry logic based on the SystemConfig/WaitPeriod.
//------------------------------------------------------------------------------
int TableInfo::acquireTableLock(bool disableTimeOut)
{
  // Save DBRoot list at start of job; used to compare at EOJ.
  Config::getRootIdList(fOrigDbRootIds);

  // If executing distributed (mode1) or central command (mode2) then
  // don't worry about table locks.  The client front-end will manage locks.
  if ((fBulkMode == BULK_MODE_REMOTE_SINGLE_SRC) || (fBulkMode == BULK_MODE_REMOTE_MULTIPLE_SRC))
  {
    if (fLog->isDebug(DEBUG_1))
    {
      ostringstream oss;
      oss << "Bypass acquiring table lock in distributed mode, "
             "for table"
          << fTableName << "; OID-" << fTableOID;
      fLog->logMsg(oss.str(), MSGLVL_INFO2);
    }

    return NO_ERROR;
  }

  const int SLEEP_INTERVAL = 100;    // sleep 100 milliseconds between checks
  const int NUM_TRIES_PER_SEC = 10;  // try 10 times per second

  int waitSeconds = Config::getWaitPeriod();
  const int NUM_TRIES = NUM_TRIES_PER_SEC * waitSeconds;
  std::string tblLockErrMsg;

  // Retry loop to lock the db table associated with this TableInfo object
  std::string processName;
  uint32_t processId;
  int32_t sessionId;
  int32_t transId;
  ostringstream pmModOss;
  pmModOss << " (pm" << Config::getLocalModuleID() << ')';
  bool timeout = false;
  // for (int i=0; i<NUM_TRIES; i++)
  int try_count = 0;

  while (!timeout)
  {
    processName = fProcessName;
    processName += pmModOss.str();
    processId = ::getpid();
    sessionId = -1;
    transId = -1;
    int rc = BRMWrapper::getInstance()->getTableLock(fTableOID, processName, processId, sessionId, transId,
                                                     fTableLockID, tblLockErrMsg);

    if ((rc == NO_ERROR) && (fTableLockID > 0))
    {
      fTableLocked = true;

      if (fLog->isDebug(DEBUG_1))
      {
        ostringstream oss;
        oss << "Table lock acquired for table " << fTableName << "; OID-" << fTableOID << "; lockID-"
            << fTableLockID;
        fLog->logMsg(oss.str(), MSGLVL_INFO2);
      }

      return NO_ERROR;
    }
    else if (fTableLockID == 0)
    {
      // sleep and then go back and try getting table lock again
      sleepMS(SLEEP_INTERVAL);

      if (fLog->isDebug(DEBUG_1))
      {
        ostringstream oss;
        oss << "Retrying to acquire table lock for table " << fTableName << "; OID-" << fTableOID;
        fLog->logMsg(oss.str(), MSGLVL_INFO2);
      }
    }
    else
    {
      ostringstream oss;
      oss << "Error in acquiring table lock for table " << fTableName << "; OID-" << fTableOID << "; "
          << tblLockErrMsg;
      fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);

      return rc;
    }

    // if disableTimeOut is set then no timeout for table lock. Forever wait....
    timeout = (disableTimeOut ? false : (++try_count >= NUM_TRIES));
  }

  ostringstream oss;
  oss << "Unable to acquire lock for table " << fTableName << "; OID-" << fTableOID
      << "; table currently locked by process-" << processName << "; pid-" << processId << "; session-"
      << sessionId << "; txn-" << transId;
  fLog->logMsg(oss.str(), ERR_TBLLOCK_GET_LOCK_LOCKED, MSGLVL_ERROR);

  return ERR_TBLLOCK_GET_LOCK_LOCKED;
}

//------------------------------------------------------------------------------
// Change table lock state (to cleanup)
//------------------------------------------------------------------------------
int TableInfo::changeTableLockState()
{
  // If executing distributed (mode1) or central command (mode2) then
  // don't worry about table locks.  The client front-end will manage locks.
  if ((fBulkMode == BULK_MODE_REMOTE_SINGLE_SRC) || (fBulkMode == BULK_MODE_REMOTE_MULTIPLE_SRC))
  {
    return NO_ERROR;
  }

  std::string tblLockErrMsg;
  bool bChanged = false;

  int rc =
      BRMWrapper::getInstance()->changeTableLockState(fTableLockID, BRM::CLEANUP, bChanged, tblLockErrMsg);

  if (rc == NO_ERROR)
  {
    if (fLog->isDebug(DEBUG_1))
    {
      ostringstream oss;

      if (bChanged)
      {
        oss << "Table lock state changed to CLEANUP for table " << fTableName << "; OID-" << fTableOID
            << "; lockID-" << fTableLockID;
      }
      else
      {
        oss << "Table lock state not changed to CLEANUP for table " << fTableName << "; OID-" << fTableOID
            << "; lockID-" << fTableLockID << ".  Table lot locked.";
      }

      fLog->logMsg(oss.str(), MSGLVL_INFO2);
    }
  }
  else
  {
    ostringstream oss;
    oss << "Error in changing table state for table " << fTableName << "; OID-" << fTableOID << "; lockID-"
        << fTableLockID << "; " << tblLockErrMsg;
    fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
    return rc;
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Releases DB table lock assigned to this TableInfo object.
//------------------------------------------------------------------------------
int TableInfo::releaseTableLock()
{
  // If executing distributed (mode1) or central command (mode2) then
  // don't worry about table locks.  The client front-end will manage locks.
  if ((fBulkMode == BULK_MODE_REMOTE_SINGLE_SRC) || (fBulkMode == BULK_MODE_REMOTE_MULTIPLE_SRC))
  {
    if (fLog->isDebug(DEBUG_1))
    {
      ostringstream oss;
      oss << "Bypass releasing table lock in distributed mode, "
             "for table "
          << fTableName << "; OID-" << fTableOID;
      fLog->logMsg(oss.str(), MSGLVL_INFO2);
    }

    return NO_ERROR;
  }

  std::string tblLockErrMsg;
  bool bReleased = false;

  // Unlock the database table
  int rc = BRMWrapper::getInstance()->releaseTableLock(fTableLockID, bReleased, tblLockErrMsg);

  if (rc == NO_ERROR)
  {
    fTableLocked = false;

    if (fLog->isDebug(DEBUG_1))
    {
      ostringstream oss;

      if (bReleased)
      {
        oss << "Table lock released for table " << fTableName << "; OID-" << fTableOID << "; lockID-"
            << fTableLockID;
      }
      else
      {
        oss << "Table lock not released for table " << fTableName << "; OID-" << fTableOID << "; lockID-"
            << fTableLockID << ".  Table not locked.";
      }

      fLog->logMsg(oss.str(), MSGLVL_INFO2);
    }
  }
  else
  {
    ostringstream oss;
    oss << "Error in releasing table lock for table " << fTableName << "; OID-" << fTableOID << "; lockID-"
        << fTableLockID << "; " << tblLockErrMsg;
    fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
    return rc;
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Delete bulk rollback metadata file.
//------------------------------------------------------------------------------
void TableInfo::deleteMetaDataRollbackFile()
{
  // If executing distributed (mode1) or central command (mode2) then
  // don't worry about table locks, or deleting meta data files.  The
  // client front-end will manage these tasks after all imports are finished.
  if ((fBulkMode == BULK_MODE_REMOTE_SINGLE_SRC) || (fBulkMode == BULK_MODE_REMOTE_MULTIPLE_SRC))
  {
    return;
  }

  if (!fKeepRbMetaFile)
  {
    // Treat any error as non-fatal, though we log it.
    try
    {
      fRBMetaWriter.deleteFile();
    }
    catch (WeException& ex)
    {
      ostringstream oss;
      oss << "Error deleting meta file; " << ex.what();
      fLog->logMsg(oss.str(), ex.errorCode(), MSGLVL_ERROR);
    }
  }
}

//------------------------------------------------------------------------------
// Changes to "existing" DB files must be confirmed on HDFS system.
// This function triggers this action.
//------------------------------------------------------------------------------
// @bug 5572 - Add db file confirmation for HDFS
int TableInfo::confirmDBFileChanges()
{
  // Unlike deleteTempDBFileChanges(), note that confirmDBFileChanges()
  // executes regardless of the import mode.  We go ahead and confirm
  // the file changes at the end of a successful cpimport.bin.
  if (idbdatafile::IDBPolicy::useHdfs())
  {
    ostringstream oss;
    oss << "Confirming DB file changes for " << fTableName;
    fLog->logMsg(oss.str(), MSGLVL_INFO2);

    std::string errMsg;
    ConfirmHdfsDbFile confirmHdfs;
    int rc = confirmHdfs.confirmDbFileListFromMetaFile(fTableOID, errMsg);

    if (rc != NO_ERROR)
    {
      ostringstream ossErrMsg;
      ossErrMsg << "Unable to confirm changes to table " << fTableName << "; " << errMsg;
      fLog->logMsg(ossErrMsg.str(), rc, MSGLVL_ERROR);

      return rc;
    }
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Temporary swap files must be deleted on HDFS system.
// This function triggers this action.
//------------------------------------------------------------------------------
// @bug 5572 - Add db file confirmation for HDFS
void TableInfo::deleteTempDBFileChanges()
{
  // If executing distributed (mode1) or central command (mode2) then
  // no action necessary.  The client front-end will initiate the deletion
  // of the temp files, only after all the distributed imports have
  // successfully completed.
  if ((fBulkMode == BULK_MODE_REMOTE_SINGLE_SRC) || (fBulkMode == BULK_MODE_REMOTE_MULTIPLE_SRC))
  {
    return;
  }

  if (idbdatafile::IDBPolicy::useHdfs())
  {
    ostringstream oss;
    oss << "Deleting DB temp swap files for " << fTableName;
    fLog->logMsg(oss.str(), MSGLVL_INFO2);

    std::string errMsg;
    ConfirmHdfsDbFile confirmHdfs;
    int rc = confirmHdfs.endDbFileListFromMetaFile(fTableOID, true, errMsg);

    // Treat any error as non-fatal, though we log it.
    if (rc != NO_ERROR)
    {
      ostringstream ossErrMsg;
      ossErrMsg << "Unable to delete temp swap files for table " << fTableName << "; " << errMsg;
      fLog->logMsg(ossErrMsg.str(), rc, MSGLVL_ERROR);
    }
  }
}

//------------------------------------------------------------------------------
// Validates the correctness of the current HWMs for this table.
// The HWMs for all the 1 byte columns should be identical.  Same goes
// for all the 2 byte columns, etc.  The 2 byte column HWMs should be
// "roughly" (but not necessarily exactly) twice that of a 1 byte column.
// Same goes for the 4 byte column HWMs vs their 2 byte counterparts, etc.
// jobTable - table/column information to use with validation.
//            We use jobTable.colList[] (if provided) instead of data memmber
//            fColumns, because this function is called during preprocessing,
//            before TableInfo.fColumns has been initialized with data from
//            colList.
// segFileInfo - Vector of File objects carrying current DBRoot, partition,
//            HWM, etc to be validated for the columns belonging to jobTable.
// stage    - Current stage we are validating.  "Starting" or "Ending".
//------------------------------------------------------------------------------
int TableInfo::validateColumnHWMs(const JobTable* jobTable, const std::vector<DBRootExtentInfo>& segFileInfo,
                                  const char* stage)
{
  int rc = NO_ERROR;

  // Used to track first 1-byte, 2-byte, 4-byte, and 8-byte columns in table
  int byte1First = -1;
  int byte2First = -1;
  int byte4First = -1;
  int byte8First = -1;
  int byte16First = -1;

  // Make sure the HWMs for all 1-byte columns match; same for all 2-byte,
  // 4-byte, and 8-byte columns as well.
  for (unsigned k = 0; k < segFileInfo.size(); k++)
  {
    int k1 = 0;

    // Validate HWMs in jobTable if we have it, else use fColumns.
    const JobColumn& jobColK = ((jobTable != 0) ? jobTable->colList[k] : fColumns[k].column);

    // Find the first 1-byte, 2-byte, 4-byte, and 8-byte columns.
    // Use those as our reference HWM for the respective column widths.
    switch (jobColK.width)
    {
      case 1:
      {
        if (byte1First == -1)
          byte1First = k;

        k1 = byte1First;
        break;
      }

      case 2:
      {
        if (byte2First == -1)
          byte2First = k;

        k1 = byte2First;
        break;
      }

      case 4:
      {
        if (byte4First == -1)
          byte4First = k;

        k1 = byte4First;
        break;
      }

      case 8:
      {
        if (byte8First == -1)
          byte8First = k;

        k1 = byte8First;
        break;
      }
      case 16:
      {
        if (byte16First == -1)
          byte16First = k;

        k1 = byte16First;
        break;
      }
      default:
      {
        ostringstream oss;
        oss << stage
            << " Unsupported width for"
               " OID-"
            << jobColK.mapOid << "; column-" << jobColK.colName << "; width-" << jobColK.width;
        fLog->logMsg(oss.str(), ERR_BRM_UNSUPP_WIDTH, MSGLVL_ERROR);
        return ERR_BRM_UNSUPP_WIDTH;
      }
    }  // end of switch based on column width.

    // Validate HWMs in jobTable if we have it, else use fColumns.
    const JobColumn& jobColK1 = ((jobTable != 0) ? jobTable->colList[k1] : fColumns[k1].column);

    // std::cout << "dbg: comparing0 " << stage << " refcol-" << k1 <<
    //  "; wid-" << jobColK1.width << "; hwm-" << segFileInfo[k1].fLocalHwm <<
    //  " <to> col-" << k <<
    //  "; wid-" << jobColK.width << " ; hwm-"<<segFileInfo[k].fLocalHwm<<std::endl;

    // Validate that the HWM for this column (k) matches that of the
    // corresponding reference column with the same width.
    if ((segFileInfo[k1].fDbRoot != segFileInfo[k].fDbRoot) ||
        (segFileInfo[k1].fPartition != segFileInfo[k].fPartition) ||
        (segFileInfo[k1].fSegment != segFileInfo[k].fSegment) ||
        (segFileInfo[k1].fLocalHwm != segFileInfo[k].fLocalHwm))
    {
      ostringstream oss;
      oss << stage
          << " HWMs do not match for"
             " OID1-"
          << jobColK1.mapOid << "; column-" << jobColK1.colName << "; DBRoot-" << segFileInfo[k1].fDbRoot
          << "; partition-" << segFileInfo[k1].fPartition << "; segment-" << segFileInfo[k1].fSegment
          << "; hwm-" << segFileInfo[k1].fLocalHwm << "; width-" << jobColK1.width << ':' << std::endl
          << " and OID2-" << jobColK.mapOid << "; column-" << jobColK.colName << "; DBRoot-"
          << segFileInfo[k].fDbRoot << "; partition-" << segFileInfo[k].fPartition << "; segment-"
          << segFileInfo[k].fSegment << "; hwm-" << segFileInfo[k].fLocalHwm << "; width-" << jobColK.width;
      fLog->logMsg(oss.str(), ERR_BRM_HWMS_NOT_EQUAL, MSGLVL_ERROR);
      return ERR_BRM_HWMS_NOT_EQUAL;
    }

    // HWM DBRoot, partition, and segment number should match for all
    // columns; so compare DBRoot, part#, and seg# with first column.
    if ((segFileInfo[0].fDbRoot != segFileInfo[k].fDbRoot) ||
        (segFileInfo[0].fPartition != segFileInfo[k].fPartition) ||
        (segFileInfo[0].fSegment != segFileInfo[k].fSegment))
    {
      const JobColumn& jobCol0 = ((jobTable != 0) ? jobTable->colList[0] : fColumns[0].column);

      ostringstream oss;
      oss << stage
          << " HWM DBRoot,Part#, or Seg# do not match for"
             " OID1-"
          << jobCol0.mapOid << "; column-" << jobCol0.colName << "; DBRoot-" << segFileInfo[0].fDbRoot
          << "; partition-" << segFileInfo[0].fPartition << "; segment-" << segFileInfo[0].fSegment
          << "; hwm-" << segFileInfo[0].fLocalHwm << "; width-" << jobCol0.width << ':' << std::endl
          << " and OID2-" << jobColK.mapOid << "; column-" << jobColK.colName << "; DBRoot-"
          << segFileInfo[k].fDbRoot << "; partition-" << segFileInfo[k].fPartition << "; segment-"
          << segFileInfo[k].fSegment << "; hwm-" << segFileInfo[k].fLocalHwm << "; width-" << jobColK.width;
      fLog->logMsg(oss.str(), ERR_BRM_HWMS_NOT_EQUAL, MSGLVL_ERROR);
      return ERR_BRM_HWMS_NOT_EQUAL;
    }
  }  // end of loop to compare all 1-byte HWMs, 2-byte HWMs, etc.

  // Validate/compare HWM for 1-byte column in relation to 2-byte column, etc.
  // Without knowing the exact row count, we can't extrapolate the exact HWM
  // for the wider column, but we can narrow it down to an expected range.
  int refCol = 0;
  int colIdx = 0;

  // Validate/compare HWMs given a 1-byte column as a starting point
  if (byte1First >= 0)
  {
    refCol = byte1First;

    if ((rc = compareHWMs(byte1First, byte2First, 1, 2, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte1First, byte4First, 1, 4, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte1First, byte8First, 1, 8, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte1First, byte16First, 1, 16, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }
  }

  // Validate/compare HWMs given a 2-byte column as a starting point
  if (byte2First >= 0)
  {
    refCol = byte2First;

    if ((rc = compareHWMs(byte2First, byte4First, 2, 4, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte2First, byte8First, 2, 8, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte2First, byte16First, 2, 16, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }
  }

  // Validate/compare HWMs given a 4-byte column as a starting point
  if (byte4First >= 0)
  {
    refCol = byte4First;

    if ((rc = compareHWMs(byte4First, byte8First, 4, 8, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }

    if ((rc = compareHWMs(byte4First, byte16First, 4, 16, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }
  }
  if (byte8First >= 0)
  {
    refCol = byte8First;
    if ((rc = compareHWMs(byte8First, byte16First, 8, 16, segFileInfo, colIdx) != NO_ERROR))
    {
      goto errorCheck;
    }
  }

// To avoid repeating this message 6 times in the preceding source code, we
// use the "dreaded" goto to branch to this single place for error handling.
errorCheck:

  if (rc != NO_ERROR)
  {
    const JobColumn& jobColRef = ((jobTable != 0) ? jobTable->colList[refCol] : fColumns[refCol].column);
    const JobColumn& jobColIdx = ((jobTable != 0) ? jobTable->colList[colIdx] : fColumns[colIdx].column);

    ostringstream oss;
    oss << stage
        << " HWMs are not in sync for"
           " OID1-"
        << jobColRef.mapOid << "; column-" << jobColRef.colName << "; DBRoot-" << segFileInfo[refCol].fDbRoot
        << "; partition-" << segFileInfo[refCol].fPartition << "; segment-" << segFileInfo[refCol].fSegment
        << "; hwm-" << segFileInfo[refCol].fLocalHwm << "; width-" << jobColRef.width << ':' << std::endl
        << " and OID2-" << jobColIdx.mapOid << "; column-" << jobColIdx.colName << "; DBRoot-"
        << segFileInfo[colIdx].fDbRoot << "; partition-" << segFileInfo[colIdx].fPartition << "; segment-"
        << segFileInfo[colIdx].fSegment << "; hwm-" << segFileInfo[colIdx].fLocalHwm << "; width-"
        << jobColIdx.width;
    fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
  }

  return rc;
}

//------------------------------------------------------------------------------
// DESCRIPTION:
//    Initialize the bulk rollback metadata writer for this table.
// RETURN:
//    NO_ERROR if success
//    other if fail
//------------------------------------------------------------------------------
int TableInfo::initBulkRollbackMetaData()
{
  int rc = NO_ERROR;

  try
  {
    fRBMetaWriter.init(fTableOID, fTableName);
  }
  catch (WeException& ex)
  {
    fLog->logMsg(ex.what(), ex.errorCode(), MSGLVL_ERROR);
    rc = ex.errorCode();
  }

  return rc;
}

//------------------------------------------------------------------------------
// DESCRIPTION:
//    Saves snapshot of extentmap into a bulk rollback meta data file, for
//    use in a bulk rollback, if the current cpimport.bin job should fail.
//    The source code in RBMetaWriter::saveBulkRollbackMetaData() used to
//    reside in this TableInfo function.  But much of the source code was
//    factored out to create RBMetaWriter::saveBulkRollbackMetaData(), so
//    that the function would reside in the shared library for reuse by DML.
// PARAMETERS:
//    job - current job
//    segFileInfo - Vector of File objects carrying starting DBRoot, partition,
//                  etc, for each column belonging to tableNo.
//    dbRootHWMInfoVecCol - vector of last local HWM info for each DBRoot
//        (asssigned to current PM) for each column in "this" table.
// RETURN:
//    NO_ERROR if success
//    other if fail
//------------------------------------------------------------------------------
int TableInfo::saveBulkRollbackMetaData(Job& job, const std::vector<DBRootExtentInfo>& segFileInfo,
                                        const std::vector<BRM::EmDbRootHWMInfo_v>& dbRootHWMInfoVecCol)
{
  int rc = NO_ERROR;

  std::vector<Column> cols;
  std::vector<OID> dctnryOids;

  // Loop through the columns in the specified table
  for (size_t i = 0; i < job.jobTableList[fTableId].colList.size(); i++)
  {
    JobColumn& jobCol = job.jobTableList[fTableId].colList[i];

    Column col;
    col.colNo = i;
    col.colWidth = jobCol.width;
    col.colType = jobCol.weType;
    col.colDataType = jobCol.dataType;
    col.dataFile.oid = jobCol.mapOid;
    col.dataFile.fid = jobCol.mapOid;
    col.dataFile.hwm = segFileInfo[i].fLocalHwm;  // starting HWM
    col.dataFile.pFile = 0;
    col.dataFile.fPartition = segFileInfo[i].fPartition;  // starting Part#
    col.dataFile.fSegment = segFileInfo[i].fSegment;      // starting seg#
    col.dataFile.fDbRoot = segFileInfo[i].fDbRoot;        // starting DBRoot
    col.compressionType = jobCol.compressionType;
    cols.push_back(col);

    OID dctnryOid = 0;

    if (jobCol.colType == COL_TYPE_DICT)
      dctnryOid = jobCol.dctnry.dctnryOid;

    dctnryOids.push_back(dctnryOid);

  }  // end of loop through columns

  fRBMetaWriter.setUIDGID(this);

  try
  {
    fRBMetaWriter.saveBulkRollbackMetaData(cols, dctnryOids, dbRootHWMInfoVecCol);
  }
  catch (WeException& ex)
  {
    fLog->logMsg(ex.what(), ex.errorCode(), MSGLVL_ERROR);
    rc = ex.errorCode();
  }

  return rc;
}

//------------------------------------------------------------------------------
// Synchronize system catalog auto-increment next value with BRM.
// This function is called at the end of normal processing to get the system
// catalog back in line with the latest auto increment next value generated by
// BRM.
//------------------------------------------------------------------------------
int TableInfo::synchronizeAutoInc()
{
  for (unsigned i = 0; i < fColumns.size(); ++i)
  {
    if (fColumns[i].column.autoIncFlag)
    {
      // TBD: Do we rollback flush cache error for autoinc.
      // Not sure we should bail out and rollback on a
      // ERR_BLKCACHE_FLUSH_LIST error, but we currently
      // rollback for "any" updateNextValue() error
      int rc = fColumns[i].finishAutoInc();

      if (rc != NO_ERROR)
      {
        return rc;
      }

      break;  // okay to break; only 1 autoinc column per table
    }
  }

  return NO_ERROR;
}

//------------------------------------------------------------------------------
// Rollback changes made to "this" table by the current import job, delete the
// meta-data files, and release the table lock.  This function only applies to
// mode3 import.  Table lock and bulk rollbacks are managed by parent cpimport
// (file splitter) process for mode1 and mode2.
//------------------------------------------------------------------------------
int TableInfo::rollbackWork()
{
  // Close any column or store files left open by abnormal termination.
  // We want to do this before reopening the files and doing a bulk rollback.
  closeOpenDbFiles();

  // Abort "local" bulk rollback if a DBRoot from the start of the job, is
  // now missing.  User should run cleartablelock to execute a rollback on
  // this PM "and" the PM where the DBRoot was moved to.
  std::vector<uint16_t> dbRootIds;
  Config::getRootIdList(dbRootIds);

  for (unsigned int j = 0; j < fOrigDbRootIds.size(); j++)
  {
    bool bFound = false;

    for (unsigned int k = 0; k < dbRootIds.size(); k++)
    {
      if (fOrigDbRootIds[j] == dbRootIds[k])
      {
        bFound = true;
        break;
      }
    }

    if (!bFound)
    {
      ostringstream oss;
      oss << "Mode3 bulk rollback not performed for table " << fTableName << "; DBRoot" << fOrigDbRootIds[j]
          << " moved from this PM during bulk load. "
          << " Run cleartablelock to rollback and release the table lock "
          << "across PMs.";
      int rc = ERR_BULK_ROLLBACK_MISS_ROOT;
      fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
      return rc;
    }
  }

  // Restore/rollback the DB files if we got far enough to begin processing
  // this table.
  int rc = NO_ERROR;

  if (hasProcessingBegun())
  {
    BulkRollbackMgr rbMgr(fTableOID, fTableLockID, fTableName, fProcessName, fLog);

    rc = rbMgr.rollback(fKeepRbMetaFile);

    if (rc != NO_ERROR)
    {
      ostringstream oss;
      oss << "Error rolling back table " << fTableName << "; " << rbMgr.getErrorMsg();
      fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
      return rc;
    }
  }

  // Delete the meta data files after rollback is complete
  deleteMetaDataRollbackFile();

  // Release the table lock
  rc = releaseTableLock();

  if (rc != NO_ERROR)
  {
    ostringstream oss;
    oss << "Table lock not cleared for table " << fTableName;
    fLog->logMsg(oss.str(), rc, MSGLVL_ERROR);
    return rc;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Allocate extent from BRM (through the stripe allocator).
//------------------------------------------------------------------------------
int TableInfo::allocateBRMColumnExtent(OID columnOID, uint16_t dbRoot, uint32_t& partition, uint16_t& segment,
                                       BRM::LBID_t& startLbid, int& allocSize, HWM& hwm, std::string& errMsg)
{
  int rc = fExtentStrAlloc.allocateExtent(columnOID, dbRoot, partition, segment, startLbid, allocSize, hwm,
                                          errMsg);
  // fExtentStrAlloc.print();

  return rc;
}

}  // namespace WriteEngine
// end of namespace
