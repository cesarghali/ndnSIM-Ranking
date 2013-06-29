/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2011 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 */

#ifndef NDN_CONTENT_STORE_IMPL_H_
#define NDN_CONTENT_STORE_IMPL_H_

#include "ndn-content-store.h"
#include "ns3/packet.h"
#include "ns3/ndn-interest.h"
#include "ns3/ndn-content-object.h"
#include <boost/foreach.hpp>
#include <boost/unordered_map.hpp>

#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/string.h"
#include "ns3/double.h"
#include <ns3/nstime.h>

#include "../../utils/trie/trie-with-policy.h"


namespace ns3 {
namespace ndn {
namespace cs {

template<class CS>
class EntryImpl : public Entry
{
public:
  typedef Entry base_type;

public:
  EntryImpl (Ptr<ContentStore> cs, Ptr<const ContentObject> header, Ptr<const Packet> packet)
    : Entry (cs, header, packet)
    , item_ (0)
  {
  }

  void
  SetTrie (typename CS::super::iterator item)
  {
    item_ = item;
  }

  typename CS::super::iterator to_iterator () { return item_; }
  typename CS::super::const_iterator to_iterator () const { return item_; }

private:
  typename CS::super::iterator item_;
};



template<class Policy>
class ContentStoreImpl : public ContentStore,
                         protected ndnSIM::trie_with_policy< Name,
                                                             ndnSIM::smart_pointer_payload_traits< EntryImpl< ContentStoreImpl< Policy > >, Entry >,
                                                             Policy >
{
public:
  typedef ndnSIM::trie_with_policy< Name,
                                    ndnSIM::smart_pointer_payload_traits< EntryImpl< ContentStoreImpl< Policy > >, Entry >,
                                    Policy > super;

  typedef EntryImpl< ContentStoreImpl< Policy > > entry;

  static TypeId
  GetTypeId ();

  ContentStoreImpl () { };
  virtual ~ContentStoreImpl () { };

  // from ContentStore

  virtual inline boost::tuple<Ptr<Packet>, Ptr<const ContentObject>, Ptr<const Packet> >
  Lookup (Ptr<const Interest> interest);

  virtual inline bool
  Add (Ptr<const ContentObject> header, Ptr<const Packet> packet);

  // virtual bool
  // Remove (Ptr<Interest> header);

  virtual inline void
  Print (std::ostream &os) const;

  virtual uint32_t
  GetSize () const;

  virtual Ptr<Entry>
  Begin ();

  virtual Ptr<Entry>
  End ();

  virtual Ptr<Entry>
  Next (Ptr<Entry>);

private:
  void
  SetMaxSize (uint32_t maxSize);

  uint32_t
  GetMaxSize () const;

  void
  SetRateAtTimeout (double rateAtTimeout);

  double
  GetRateAtTimeout () const;

private:
  static LogComponent g_log; ///< @brief Logging variable
  boost::unordered_map<std::string, int> name_count;
  double rate_at_timeout;

  /// @brief trace of for entry additions (fired every time entry is successfully added to the cache): first parameter is pointer to the CS entry
  TracedCallback< Ptr<const Entry> > m_didAddEntry;
};

//////////////////////////////////////////
////////// Implementation ////////////////
//////////////////////////////////////////


template<class Policy>
LogComponent ContentStoreImpl< Policy >::g_log = LogComponent (("ndn.cs." + Policy::GetName ()).c_str ());


template<class Policy>
TypeId
ContentStoreImpl< Policy >::GetTypeId ()
{
  static TypeId tid = TypeId (("ns3::ndn::cs::"+Policy::GetName ()).c_str ())
    .SetGroupName ("Ndn")
    .SetParent<ContentStore> ()
    .AddConstructor< ContentStoreImpl< Policy > > ()
    .AddAttribute ("MaxSize",
                   "Set maximum number of entries in ContentStore. If 0, limit is not enforced",
                   StringValue ("100"),
                   MakeUintegerAccessor (&ContentStoreImpl< Policy >::GetMaxSize,
                                         &ContentStoreImpl< Policy >::SetMaxSize),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("RateAtTimeout",
                   "Set the cache entry rate at timeout",
                   DoubleValue (0.01),
                   MakeDoubleAccessor (&ContentStoreImpl< Policy >::GetRateAtTimeout,
                                       &ContentStoreImpl< Policy >::SetRateAtTimeout),
                   MakeDoubleChecker<double> ())

    .AddTraceSource ("DidAddEntry", "Trace fired every time entry is successfully added to the cache",
                     MakeTraceSourceAccessor (&ContentStoreImpl< Policy >::m_didAddEntry))
    ;

  return tid;
}

template<class Policy>
boost::tuple<Ptr<Packet>, Ptr<const ContentObject>, Ptr<const Packet> >
ContentStoreImpl<Policy>::Lookup (Ptr<const Interest> interest)
{
  NS_LOG_FUNCTION (this << interest->GetName ());

  // Read the exclusion filter if exists in the interest
  ns3::Ptr<const Exclusion> exclusionFilter = interest->GetExclusionPtr ();

  // Process statistics about this content name
  int count = 1;
  std::pair<boost::unordered_map<std::string, int>::iterator, bool> pair = name_count.insert(std::make_pair (interest->GetName ().ToString (), count));
  if (pair.second == false)
    {
      pair.first->second++;
      count = pair.first->second;
    }

  /// @todo Change to search with predicate
  typename super::const_iterator node = this->deepest_prefix_match (interest->GetName (), exclusionFilter, count);

  if (node != this->end ())
    {
      this->m_cacheHitsTrace (interest, node->payload ()->GetHeader ());

      // NS_LOG_DEBUG ("cache hit with " << node->payload ()->GetHeader ()->GetName ());
      return boost::make_tuple (node->payload ()->GetFullyFormedNdnPacket (),
                                node->payload ()->GetHeader (),
                                node->payload ()->GetPacket ());
    }
  else
    {
      // NS_LOG_DEBUG ("cache miss for " << interest->GetName ());
      this->m_cacheMissesTrace (interest);
      return boost::tuple<Ptr<Packet>, Ptr<ContentObject>, Ptr<Packet> > (0, 0, 0);
    }
}

template<class Policy>
bool
ContentStoreImpl<Policy>::Add (Ptr<const ContentObject> header, Ptr<const Packet> packet)
{
  NS_LOG_FUNCTION (this << header->GetName ());

  // Calculate the SHA_1 has of the content object
  std::string strhash = header->GetHash ();

  Ptr< entry > newEntry = Create< entry > (this, header, packet);
  std::pair< typename super::iterator, bool > result = super::insert (header->GetName (), newEntry, const_cast<char*>(strhash.c_str()), header->GetFreshness ().GetSeconds (),
                                                                      rate_at_timeout);

  if (result.first != super::end ())
    {
      if (result.second)
        {
          newEntry->SetTrie (result.first);

          m_didAddEntry (newEntry);
          return true;
        }
      else
        {
          // should we do anything?
          // update payload? add new payload?
          return false;
        }
    }
  else
    return false; // cannot insert entry
}

template<class Policy>
void
ContentStoreImpl<Policy>::Print (std::ostream &os) const
{
  for (typename super::policy_container::const_iterator item = this->getPolicy ().begin ();
       item != this->getPolicy ().end ();
       item++)
    {
      os << item->payload ()->GetName () << std::endl;
    }
}

template<class Policy>
void
ContentStoreImpl<Policy>::SetMaxSize (uint32_t maxSize)
{
  this->getPolicy ().set_max_size (maxSize);
}

template<class Policy>
uint32_t
ContentStoreImpl<Policy>::GetMaxSize () const
{
  return this->getPolicy ().get_max_size ();
}

template<class Policy>
void
ContentStoreImpl<Policy>::SetRateAtTimeout (double rateAtTimeout)
{
  this->rate_at_timeout = rateAtTimeout;
}

template<class Policy>
double
ContentStoreImpl<Policy>::GetRateAtTimeout () const
{
  return this->rate_at_timeout;
}

template<class Policy>
uint32_t
ContentStoreImpl<Policy>::GetSize () const
{
  return this->getPolicy ().size ();
}

template<class Policy>
Ptr<Entry>
ContentStoreImpl<Policy>::Begin ()
{
  typename super::parent_trie::recursive_iterator item (super::getTrie ()), end (0);
  for (; item != end; item++)
    {
      if (item->payload () == 0) continue;
      break;
    }

  if (item == end)
    return End ();
  else
    return item->payload ();
}

template<class Policy>
Ptr<Entry>
ContentStoreImpl<Policy>::End ()
{
  return 0;
}

template<class Policy>
Ptr<Entry>
ContentStoreImpl<Policy>::Next (Ptr<Entry> from)
{
  if (from == 0) return 0;

  typename super::parent_trie::recursive_iterator
    item (*StaticCast< entry > (from)->to_iterator ()),
    end (0);

  for (item++; item != end; item++)
    {
      if (item->payload () == 0) continue;
      break;
    }

  if (item == end)
    return End ();
  else
    return item->payload ();
}


} // namespace cs
} // namespace ndn
} // namespace ns3

#endif // NDN_CONTENT_STORE_IMPL_H_
