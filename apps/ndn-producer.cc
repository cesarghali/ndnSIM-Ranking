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
 * Author: Ilya Moiseenko <iliamo@cs.ucla.edu>
 *         Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 */

#include "ndn-producer.h"
#include "ns3/log.h"
#include "ns3/ndn-interest.h"
#include "ns3/ndn-content-object.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include "ns3/ndn-app-face.h"
#include "ns3/ndn-fib.h"

#include "ns3/ndnSIM/utils/ndn-fw-hop-count-tag.h"

#include <boost/ref.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>

#include <sys/time.h>
namespace ll = boost::lambda;

NS_LOG_COMPONENT_DEFINE ("ndn.Producer");

namespace ns3 {
namespace ndn {

NS_OBJECT_ENSURE_REGISTERED (Producer);
    
TypeId
Producer::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::ndn::Producer")
    .SetGroupName ("Ndn")
    .SetParent<App> ()
    .AddConstructor<Producer> ()
    .AddAttribute ("Prefix","Prefix, for which producer has the data",
                   StringValue ("/"),
                   MakeNameAccessor (&Producer::m_prefix),
                   MakeNameChecker ())
    .AddAttribute ("PayloadSize", "Virtual payload size for Content packets",
                   UintegerValue (1024),
                   MakeUintegerAccessor(&Producer::m_virtualPayloadSize),
                   MakeUintegerChecker<uint32_t>())
    .AddAttribute ("Freshness", "Freshness of data packets, if 0, then unlimited freshness",
                   TimeValue (Seconds (0)),
                   MakeTimeAccessor (&Producer::m_freshness),
                   MakeTimeChecker ())
    .AddAttribute ("BadContentRate", "Indicate the probability of which the producer is going to generate a bad content (with a bogus hash)",
                   DoubleValue (0.0),
                   MakeDoubleAccessor (&Producer::m_badContentRate),
                   MakeDoubleChecker<double> ())

    .AddTraceSource ("BadContentTransmitted", "Trace called every time the producer sends a bad content",
                     MakeTraceSourceAccessor (&Producer::m_badContentTransmittedTrace))
    ;
        
  return tid;
}
    
Producer::Producer ()
{
  // NS_LOG_FUNCTION_NOARGS ();
}

// inherited from Application base class.
void
Producer::StartApplication ()
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_ASSERT (GetNode ()->GetObject<Fib> () != 0);

  App::StartApplication ();

  NS_LOG_DEBUG ("NodeID: " << GetNode ()->GetId ());
  
  Ptr<Fib> fib = GetNode ()->GetObject<Fib> ();
  
  Ptr<fib::Entry> fibEntry = fib->Add (m_prefix, m_face, 0);

  fibEntry->UpdateStatus (m_face, fib::FaceMetric::NDN_FIB_GREEN);
  
  // // make face green, so it will be used primarily
  // StaticCast<fib::FibImpl> (fib)->modify (fibEntry,
  //                                        ll::bind (&fib::Entry::UpdateStatus,
  //                                                  ll::_1, m_face, fib::FaceMetric::NDN_FIB_GREEN));
}

void
Producer::StopApplication ()
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_ASSERT (GetNode ()->GetObject<Fib> () != 0);

  App::StopApplication ();
}


void
Producer::OnInterest (const Ptr<const Interest> &interest, Ptr<Packet> origPacket)
{
  App::OnInterest (interest, origPacket); // tracing inside

  NS_LOG_FUNCTION (this << interest);

  if (!m_active) return;
    
  static ContentObjectTail tail;
  Ptr<ContentObject> header = Create<ContentObject> ();
  header->SetName (Create<Name> (interest->GetName ()));
  header->SetFreshness (m_freshness);
  
  // Add timestamp
  header->SetTimestamp(Simulator::Now());
  // Add signature
  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand((int)Simulator::Now().GetNanoSeconds() + tv.tv_usec);
  header->SetSignature(rand());
  // Computing the hash in the content header
  header->SetHash(header->ComputeHash());
  // Produce a bad content
  double r = (double)rand() / RAND_MAX;
  if (m_badContentRate != 0 && r <= m_badContentRate)
    {
      header->SetSignature(rand());
    }

  NS_LOG_INFO ("node("<< GetNode()->GetId() <<") respodning with ContentObject:\n" << boost::cref(*header));
  
  Ptr<Packet> packet = Create<Packet> (m_virtualPayloadSize);
  
  packet->AddHeader (*header);
  packet->AddTrailer (tail);

  // Echo back FwHopCountTag if exists
  FwHopCountTag hopCountTag;
  if (origPacket->RemovePacketTag (hopCountTag))
    {
      packet->AddPacketTag (hopCountTag);
    }

  m_protocolHandler (packet);
  
  m_transmittedContentObjects (header, packet, this, m_face);
  
  // Fire event bad content transmitted
  if (m_badContentRate != 0 && r <= m_badContentRate)
    {
      m_badContentTransmittedTrace(header);
    }
}

} // namespace ndn
} // namespace ns3
