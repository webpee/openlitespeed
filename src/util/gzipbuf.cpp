/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include <util/gzipbuf.h>
#include <util/vmembuf.h>
#include <util/ni_fio.h>

#include <assert.h>
//#include <netinet/in.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_FLUSH_WINDOW    2048

GzipBuf::GzipBuf()
{
    memset (this, 0, sizeof( GzipBuf ));
    m_flushWindowSize = DEFAULT_FLUSH_WINDOW;
}

GzipBuf::GzipBuf( int type, int level )
{
    memset (this, 0, sizeof( GzipBuf ));
    m_flushWindowSize = DEFAULT_FLUSH_WINDOW;
    init( type, level );    
}

GzipBuf::~GzipBuf()
{
    release();
}

int GzipBuf::release()
{
    if ( m_type == GZIP_INFLATE )
       return inflateEnd(&m_zstr);
    else
       return deflateEnd(&m_zstr);
}


int GzipBuf::init( int type, int level )
{
    int ret;
    if ( type == GZIP_INFLATE )
        m_type = GZIP_INFLATE;
    else
        m_type = GZIP_DEFLATE;
    if ( m_type == GZIP_DEFLATE )
    {
        if ( !m_zstr.state )
            ret = deflateInit2 (&m_zstr, level, Z_DEFLATED, 15+16, 8,
                            Z_DEFAULT_STRATEGY);
        else
        {
            ret = deflateReset( &m_zstr );
        }
    }
    else
    {
        if ( !m_zstr.state )
            ret = inflateInit2( &m_zstr, 15+16 );
        else
        {
            ret = inflateReset( &m_zstr );
        }
    }
    return ret;
}

int GzipBuf::reinit()
{
    int ret;
    if ( m_type == GZIP_DEFLATE )
    {
        ret = deflateReset( &m_zstr );
    }
    else
    {
        ret = inflateReset( &m_zstr );
    }
    m_streamStarted = 1;
    return ret;
}


int GzipBuf::beginStream()
{
    if ( !m_pCompressCache )
        return -1;
    size_t size;
    m_zstr.next_out = (unsigned char *)
            m_pCompressCache->getWriteBuffer( size );
    m_zstr.avail_out = size;
    if ( !m_zstr.next_out )
        return -1;
    m_streamStarted = 1;
    return 0;
}

int GzipBuf::compress( const char * pBuf, int len )
{
    if ( !m_streamStarted )
        return -1;
    m_zstr.next_in = (unsigned char *)pBuf;
    m_zstr.avail_in = len;
    return process( 0 );
}

int GzipBuf::process( int finish )
{
    do
    {
        int ret;
        size_t size;
        if ( !m_zstr.avail_out )
        {
            m_zstr.next_out = (unsigned char *)m_pCompressCache->getWriteBuffer( size );
            m_zstr.avail_out = size;
            assert( m_zstr.avail_out );
        }
        if ( m_type == GZIP_DEFLATE )
            ret = ::deflate(&m_zstr, finish);
        else
            ret = ::inflate(&m_zstr, finish );
        if ( ret == Z_STREAM_ERROR )
            return -1;
        if ( ret == Z_BUF_ERROR )
            ret = 0;
        m_pCompressCache->writeUsed( m_zstr.next_out -
                (unsigned char *)m_pCompressCache->getCurWPos() );
        if (( m_zstr.avail_out )||( ret == Z_STREAM_END ))
            return ret;
        m_zstr.next_out = (unsigned char *)m_pCompressCache->getWriteBuffer( size );
        m_zstr.avail_out = size;
        if ( !m_zstr.next_out )
            return -1;
    }while( true );
}


int GzipBuf::endStream()
{
    int ret = process( Z_FINISH );
    m_streamStarted = 0;
    if (ret != Z_STREAM_END)
    {
        return -1;
    }
    return 0;
}

int GzipBuf::resetCompressCache()
{
    m_pCompressCache->rewindReadBuf();
    m_pCompressCache->rewindWriteBuf();
    size_t size;
    m_zstr.next_out = (unsigned char *)
            m_pCompressCache->getWriteBuffer( size );
    m_zstr.avail_out = size;
    return 0;
}


int GzipBuf::processFile( int type, const char * pFileName,
                    const char * pCompressFileName )
{
    int fd;
    int ret = 0;
    fd = open( pFileName, O_RDONLY );
    if ( fd == -1 )
        return -1;
    VMemBuf gzFile;
    ret = gzFile.set( pCompressFileName, -1 );
    if ( !ret )
    {
        setCompressCache( &gzFile );
        if ( ((ret = init( type, 6 )) == 0 )&&((ret = beginStream()) == 0 ))
        {
            int len;
            char achBuf[16384];
            while( true )
            {
                len = nio_read( fd, achBuf, sizeof( achBuf ) );
                if ( len <= 0 )
                    break;
                if ( this->write( achBuf, len ) )
                {
                    ret = -1;
                    break;
                }
            }
            if ( !ret )
            {
                ret = endStream();
                long size;
                if ( !ret )
                    ret = gzFile.exactSize( &size );
            }
            gzFile.close();
        }
    }
    ::close( fd );
    return ret;
}



