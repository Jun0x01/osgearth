#include <osgEarth/HTTPClient>
#include <osgDB/Registry>
#include <osg/Notify>
#include <curl/curl.h>
#include <curl/types.h>

using namespace osgEarth;

/****************************************************************************/
   
namespace osgEarth
{
    struct StreamObject
    {
        StreamObject(std::ostream* stream1, const std::string& cacheFileName) :
            _stream1(stream1),
            _cacheFileName(cacheFileName) { }

            void write(const char* ptr, size_t realsize) {
                if (_stream1) _stream1->write(ptr, realsize);

                if (!_cacheFileName.empty())
                {
                    if (!_foutOpened)
                    {
                        osg::notify(osg::INFO)<<"Writing to cache: "<<_cacheFileName<<std::endl;
                        _fout.open(_cacheFileName.c_str(), std::ios::out | std::ios::binary);
                        _foutOpened = true;
                    }

                    if (_fout)
                    {
                        _fout.write(ptr, realsize);
                    }
                }
            }

        std::ostream*   _stream1;

        bool            _foutOpened;
        std::string     _cacheFileName;
        std::ofstream   _fout;
    };

    static size_t
    StreamObjectReadCallback(void* ptr, size_t size, size_t nmemb, void* data)
    {
        size_t realsize = size* nmemb;
        StreamObject* sp = (StreamObject*)data;
        sp->write((const char*)ptr, realsize);
        return realsize;
    }
}

/****************************************************************************/

HTTPRequest::HTTPRequest( const std::string& _url )
: url( _url )
{
    //NOP
}

void
HTTPRequest::addParameter( const std::string& name, const std::string& value )
{
    parameters[name] = value;
}

void
HTTPRequest::addParameter( const std::string& name, int value )
{
    std::stringstream buf;
    buf << value;
    parameters[name] = buf.str();
}

void
HTTPRequest::addParameter( const std::string& name, double value )
{
    std::stringstream buf;
    buf << value;
    parameters[name] = buf.str();
}

const HTTPRequest::Parameters&
HTTPRequest::getParameters() const
{
    return parameters; 
}

std::string
HTTPRequest::getURL() const
{
    std::stringstream buf;
    buf << url;
    for( Parameters::const_iterator i = parameters.begin(); i != parameters.end(); i++ )
    {
        buf << ( i == parameters.begin()? "?" : "&" );
        buf << i->first << "=" << i->second;
    }
    return buf.str();
}

/****************************************************************************/

HTTPResponse::HTTPResponse( long _code )
: response_code( _code )
{
    //NOP
}

long
HTTPResponse::getCode() const {
    return response_code;
}

bool
HTTPResponse::isOK() const {
    return response_code == 200L;
}

unsigned int
HTTPResponse::getNumParts() const {
    return parts.size();
}

unsigned int
HTTPResponse::getPartSize( unsigned int n ) const {
    return parts[n]->size;
}

const std::string&
HTTPResponse::getPartHeader( unsigned int n, const std::string& name ) const {
    return parts[n]->headers[name];
}

std::istream&
HTTPResponse::getPartStream( unsigned int n ) const {
    return parts[n]->stream;
}

/****************************************************************************/

HTTPClient::HTTPClient()
{
    proxy_port = 8080;
}

void
HTTPClient::setProxyHost( const std::string& host )
{
    proxy_host = host;
}

void
HTTPClient::setProxyPort( unsigned short port )
{
    proxy_port = port;
}

// from: http://www.rosettacode.org/wiki/Tokenizing_A_String#C.2B.2B
static std::vector<std::string> 
tokenize_str(const std::string & str, const std::string & delims=", \t")
{
  using namespace std;
  // Skip delims at beginning, find start of first token
  string::size_type lastPos = str.find_first_not_of(delims, 0);
  // Find next delimiter @ end of token
  string::size_type pos     = str.find_first_of(delims, lastPos);

  // output vector
  vector<string> tokens;

  while (string::npos != pos || string::npos != lastPos)
    {
      // Found a token, add it to the vector.
      tokens.push_back(str.substr(lastPos, pos - lastPos));
      // Skip delims.  Note the "not_of". this is beginning of token
      lastPos = str.find_first_not_of(delims, pos);
      // Find next delimiter at end of token.
      pos     = str.find_first_of(delims, lastPos);
    }

  return tokens;
}


void
HTTPClient::decodeMultipartStream(const std::string&   boundary,
                                  HTTPResponse::Part*  input,
                                  HTTPResponse::Parts& output) const
{
    std::string bstr = "--" + boundary;
    std::string line;
    char tempbuf[256];

    // first thing in the stream should be the boundary.
    input->stream.read( tempbuf, bstr.size() );
    line = tempbuf;
    if ( line != bstr )
    {
        osg::notify(osg::WARN) << "HTTPClient.decodeMultipartStream: protocol violation" << std::endl;
        return;
    }

    for( bool done=false; !done; )
    {
        osg::ref_ptr<HTTPResponse::Part> next_part = new HTTPResponse::Part();

        // first finish off the boundary.
        std::getline( input->stream, line );
        if ( line == "--" )
        {
            done = true;
        }
        else
        {
            // read all headers. this ends with a blank line.
            line = " ";
            while( line.length() > 0 && !done )
            {
                std::getline( input->stream, line );

                //osg::notify(osg::NOTICE) << "in >>> \"" << line << "\"" << std::endl;

                // check for EOS:
                if ( line == "--" )
                {
                    done = true;
                }
                else
                {
                    std::vector<std::string> tized = tokenize_str( line, ":" );
                    if ( tized.size() >= 2 )
                        next_part->headers[tized[0]] = tized[1];
                }
            }
        }

        if ( !done )
        {
            // read data until we reach the boundary
            unsigned int bstr_ptr = 0;
            std::string temp;
            //unsigned int c = 0;
            while( bstr_ptr < bstr.length() )
            {
                char b;
                input->stream.read( &b, 1 );
                //osg::notify(osg::NOTICE) << ">>>read character: " << (int)b << std::endl;
                if ( b == bstr[bstr_ptr] )
                {
                    bstr_ptr++;
                    //osg::notify(osg::NOTICE) << "BOUNDARY CHAR => " << bstr_ptr << std::endl;
                }
                else
                {
                    for( unsigned int i=0; i<bstr_ptr; i++ )
                    {
                      //  osg::notify(osg::NOTICE) << ">>>read bufracter: " << c++ << ", " <<(int)bstr[i] << std::endl;
                        next_part->stream << bstr[i];
                    }
                    //osg::notify(osg::NOTICE) << ">>>read character: " << c++ << ", " << (int)b << std::endl;
                    next_part->stream << b;
                    next_part->size += bstr_ptr + 1;
                    bstr_ptr = 0;
                }
            }
            //osg::notify(osg::NOTICE) << "FOUND BOUNDARY!" << std::endl << std::endl;
            //next_part->size = c;
            output.push_back( next_part.get() );
        }
    }
}

HTTPResponse*
HTTPClient::get( HTTPRequest* request ) const
{
    HTTPResponse* response = NULL;

    std::string proxy_addr;

    if ( !proxy_host.empty() )
    {
        std::stringstream buf;
        buf << proxy_host << ":" << proxy_port;
        proxy_addr = buf.str();
    }

    //osg::notify(osg::NOTICE) << "[HTTPClient] GET " << request->getURL() << std::endl;

    osg::ref_ptr<HTTPResponse::Part> part = new HTTPResponse::Part();
    StreamObject sp( &part->stream, std::string() );

    CURL* curl_handle = curl_easy_init();
    curl_easy_setopt( curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0" );
    curl_easy_setopt( curl_handle, CURLOPT_WRITEFUNCTION, osgEarth::StreamObjectReadCallback );
    curl_easy_setopt( curl_handle, CURLOPT_URL, request->getURL().c_str() );
    curl_easy_setopt( curl_handle, CURLOPT_WRITEDATA, (void*)&sp);
    CURLcode res = curl_easy_perform( curl_handle );
    curl_easy_setopt( curl_handle, CURLOPT_WRITEDATA, (void*)0 );
    
    if ( res == 0 )
    {
        long code;
        if ( !proxy_addr.empty() )
            curl_easy_getinfo( curl_handle, CURLINFO_HTTP_CONNECTCODE, &code );
        else
            curl_easy_getinfo( curl_handle, CURLINFO_RESPONSE_CODE, &code );     

        //osg::notify(osg::NOTICE) << "[HTTPClient] got response, code = " << code << std::endl;

        response = new HTTPResponse( code );

        // check for multipart content:
        char* content_type_cp;
        curl_easy_getinfo( curl_handle, CURLINFO_CONTENT_TYPE, &content_type_cp );
        if ( content_type_cp == NULL )
        {
            osg::notify(osg::NOTICE) << "[HTTPClient] NULL Content-Type (protocol violation)" << std::endl;
            return NULL;
        }

        std::string content_type( content_type_cp );
        //osg::notify(osg::NOTICE) << "[HTTPClient] content-type = \"" << content_type << "\"" << std::endl;
        if ( content_type == "multipart/mixed; boundary=wcs" ) //todo: parse this.
        {
            //osg::notify(osg::NOTICE) << "[HTTPClient] detected multipart data; decoding..." << std::endl;
            //TODO: parse out the "wcs" -- this is mapserver-specific
            decodeMultipartStream( "wcs", part.get(), response->parts );
            //osg::notify(osg::NOTICE) << "[HTTPClient] ....decoded " << response->parts.size() << " parts:" << std::endl;
            //for( unsigned int i=0; i<response->getNumParts(); i++ )
            //{
            //    osg::notify(osg::NOTICE) << "  Part " << i << ": " << response->getPartSize(i) << std::endl;
            //}
        }
        else
        {
            //osg::notify(osg::NOTICE) << "[HTTPClient] detected single part data" << std::endl;
            response->parts.push_back( part.get() );
        }
    }
    
    curl_easy_cleanup( curl_handle );
    curl_handle = 0;

    return response;
}

