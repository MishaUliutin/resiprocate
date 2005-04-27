#ifndef RESIP_DNS_STUB_HXX
#define RESIP_DNS_STUB_HXX

#if defined(USE_ARES)
extern "C"
{
#include "ares.h"
#include "ares_dns.h"
}
#endif

#include "resiprocate/os/Inserter.hxx"
#include "resiprocate/os/Logger.hxx"

#include "resiprocate/dns/RROverlay.hxx"
#include "resiprocate/dns/RRList.hxx"
#include "resiprocate/dns/RRCache.hxx"
#include "resiprocate/dns/DnsResourceRecord.hxx"
#include "resiprocate/dns/DnsNaptrRecord.hxx"
#include "resiprocate/dns/DnsAAAARecord.hxx"
#include "resiprocate/dns/DnsHostRecord.hxx"
#include "resiprocate/dns/DnsSrvRecord.hxx"
#include "resiprocate/dns/DnsHostRecord.hxx"
#include "resiprocate/dns/DnsCnameRecord.hxx"

namespace resip
{

class DnsInterface;

template<typename T>
class DNSResult
{
   public:
      Data domain;
      int status;
      Data msg;
      std::vector<T> records;
};

class DnsResultSink
{
   public:
      virtual void onDnsResult(const DNSResult<DnsHostRecord>&) = 0;

#ifdef USE_IPV6
      virtual void onDnsResult(const DNSResult<DnsAAAARecord>&) = 0;
#endif

      virtual void onDnsResult(const DNSResult<DnsSrvRecord>&) = 0;
      virtual void onDnsResult(const DNSResult<DnsNaptrRecord>&) = 0;
      virtual void onDnsResult(const DNSResult<DnsCnameRecord>&) = 0;
};

class DnsRawSink
{
   public:
      virtual void onDnsRaw(int statuts, const unsigned char* abuf, int len) = 0;
};

class DnsStub
{
   public:

      class DnsStubException : public BaseException
      {
         public:
            DnsStubException(const Data& msg, const Data& file, const int line)
               : BaseException(msg, file, line) 
            {
            }
            
            const char* name() const { return "DnsStubException"; }
      };

      DnsStub(DnsInterface* dns);
      ~DnsStub();

      template<class QueryType> void lookup(const Data& target, DnsResultSink* s)
      {
         Query<QueryType>* query = new Query<QueryType>(*this, target, s);
         mQueries.insert(query);
         query->go(mDns);
      }

      void setTTL(int ttl) 
      {
         mCache.setTTL(ttl);
      }
      
   protected:
      void cache(const Data& key, const unsigned char* abuf, int alen);
      void cacheTTL(const Data& key, int rrType, const unsigned char* abuf, int alen);

   private:

      class QueryBase
      {
      public:
         virtual ~QueryBase() {}
      };

      template<class QueryType> 
      class Query : public DnsRawSink, public QueryBase
      {
         public:
            Query(DnsStub& stub, const Data& target, DnsResultSink* s)
               : QueryBase(), 
                 mStub(stub), 
                 mTarget(target),
                 mReQuery(0),
                 mSink(s),
                 mDns(0)
            {
            }

            ~Query() {}

            enum {MAX_REQUERIES = 5};

            void go(DnsInterface* dns)
            {
               if (mStub.mCache.lookup(mTarget, QueryType::getRRType()).empty())
               {
                  Data targetToQuery = mTarget;
                  if (QueryType::getRRType() != T_CNAME)
                  {
                     std::vector<DnsResourceRecord*> cnames = mStub.mCache.lookup(mTarget, T_CNAME);
                     if (!cnames.empty()) targetToQuery = (dynamic_cast<DnsCnameRecord*>(cnames[0]))->cname();
                  }

                  mDns = dns;
                  if (mDns) mDns->lookupRecords(targetToQuery, QueryType::getRRType(), this);

               }
               else
               {
                  if (!mSink) return;
                  DNSResult<typename QueryType::Type> result;
                  result.domain = mTarget;
                  result.status = 0;
                  cloneRecords(result.records, mStub.mCache.lookup(mTarget, QueryType::getRRType()));
                  mSink->onDnsResult(result);
                  mStub.removeQuery(this);
                  delete this;
               }
            }

            void cloneRecords(vector<typename QueryType::Type>& records, const vector<DnsResourceRecord*>& src)
            {
               for (unsigned int i = 0; i < src.size(); ++i)
               {
                  records.push_back(*(dynamic_cast<typename QueryType::Type*>(src[i])));
               }
            }

            void process(int status, const unsigned char* abuf, const int alen)
            {
               if (status != 0)
               {
                  if (status == 4) // domain name not found.
                  {
                     mStub.cacheTTL(mTarget, QueryType::getRRType(), abuf, alen);
                  }
                  std::vector<typename QueryType::Type> Empty;
                  notifyUser(status, Empty);
                  mReQuery = 0;
                  mStub.removeQuery(this);
                  delete this;
                  return;
               }

               bool bDeleteThis = true;

               // skip header
               const unsigned char* aptr = abuf + HFIXEDSZ;

               int qdcount = DNS_HEADER_QDCOUNT(abuf); // questions.
               for (int i = 0; i < qdcount && aptr; ++i)
               {
                  aptr = mStub.skipDNSQuestion(aptr, abuf, alen);
               }

               int ancount = DNS_HEADER_ANCOUNT(abuf);
               if (ancount == 0)
               {
                  std::vector<typename QueryType::Type> Empty;
                  notifyUser(0, Empty);
               }
               else
               {
                  bool bGotAnswers = true;
                  if (ancount == 1)
                  {
                     followCname(aptr, abuf, alen, bGotAnswers, bDeleteThis);
                  }

                  if (bGotAnswers)
                  {
                     mStub.cache(mTarget, abuf, alen);
                     mReQuery = 0;
                     std::vector<typename QueryType::Type> records;
                     cloneRecords(records, mStub.mCache.lookup(mTarget, QueryType::getRRType()));
                     notifyUser(0, records);
                  }
               }
               
               if (bDeleteThis) 
               {
                  mStub.removeQuery(this);
                  delete this;
               }
            }

            void onDnsRaw(int status, const unsigned char* abuf, int alen)
            {
               process(status, abuf, alen);
            }

            void followCname(const unsigned char* aptr, const unsigned char*abuf, const int alen, bool& bGotAnswers, bool& bDeleteThis)
            {
               bGotAnswers = true;
               bDeleteThis = true;
               std::vector<typename QueryType::Type> Empty;
               if (QueryType::getRRType() != T_CNAME)
               {
                  char* name = 0;
                  int len = 0;
                  if (int status = ares_expand_name(aptr, abuf, alen, &name, &len) != ARES_SUCCESS)
                  {
                     notifyUser(status, Empty);
                  }
                  aptr += len;

                  if (DNS_RR_TYPE(aptr) == T_CNAME)
                  {
                     if (QueryType::SupportsCName && mReQuery < MAX_REQUERIES)
                     {
                        mStub.cache(mTarget, abuf, alen);
                        ++mReQuery;
                        std::vector<DnsResourceRecord*> cnames = mStub.mCache.lookup(mTarget, T_CNAME);
                        mDns->lookupRecords((dynamic_cast<DnsCnameRecord*>(cnames[0]))->cname(), QueryType::getRRType(), this);
                        bDeleteThis = false;
                     }
                     else
                     {
                        mReQuery = 0;
                        notifyUser(0, Empty);
                     }
                     bGotAnswers = false;
                  }
               }
            }

            void notifyUser(int status, std::vector<typename QueryType::Type>& records)
            {
               if (!mSink) return;
               DNSResult<typename QueryType::Type>  result;
               result.domain = mTarget;
               result.status = status;
               result.records = records;
               result.msg = mDns->errorMessage(status);
               mSink->onDnsResult(result);
            }

         private:
            DnsStub& mStub;
            Data mTarget;
            int mReQuery;
            DnsResultSink* mSink;
            DnsInterface* mDns;
      };

   private:
      DnsStub(const DnsStub&);   // disable copy ctor.

      RRCache mCache;
      const unsigned char* skipDNSQuestion(const unsigned char *aptr,
                                           const unsigned char *abuf,
                                           int alen);
      bool supportedType(int);
      const unsigned char* createOverlay(const unsigned char* abuf, 
                                         const int alen, 
                                         const unsigned char* aptr, 
                                         std::vector<RROverlay>&);

      void removeQuery(QueryBase*);

      DnsInterface* mDns;
      std::set<QueryBase*> mQueries;
};

}

#endif
 
