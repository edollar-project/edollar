// Copyright (c) 2014-2017, The Monero Project
// Copyright (c) 2017-2018, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include "unsigned_transaction.h"
#include "wallet.h"
#include "common_defines.h"

#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"

#include <memory>
#include <vector>
#include <sstream>
#include <boost/format.hpp>

using namespace std;

namespace Edollar {

UnsignedTransaction::~UnsignedTransaction() {}


UnsignedTransactionImpl::UnsignedTransactionImpl(WalletImpl &wallet)
    : m_wallet(wallet)
{
  m_status = Status_Ok;
}

UnsignedTransactionImpl::~UnsignedTransactionImpl()
{
    LOG_PRINT_L3("Unsigned tx deleted");
}

int UnsignedTransactionImpl::status() const
{
    return m_status;
}

string UnsignedTransactionImpl::errorString() const
{
    return m_errorString;
}

bool UnsignedTransactionImpl::sign(const std::string &signedFileName)
{
  if(m_wallet.watchOnly())
  {
     m_errorString = tr("This is a watch only wallet");
     m_status = Status_Error;
     return false;
  }
  std::vector<tools::wallet2::pending_tx> ptx;
  try
  {
    bool r = m_wallet.m_wallet->sign_tx(m_unsigned_tx_set, signedFileName, ptx);
    if (!r)
    {
      m_errorString = tr("Failed to sign transaction");
      m_status = Status_Error;
      return false;
    }
  }
  catch (const std::exception &e)
  {
    m_errorString = string(tr("Failed to sign transaction")) + e.what();
    m_status = Status_Error;
    return false;
  }
  return true;
}

//----------------------------------------------------------------------------------------------------
bool UnsignedTransactionImpl::checkLoadedTx(const std::function<size_t()> get_num_txes, const std::function<const tools::wallet2::tx_construction_data&(size_t)> &get_tx, const std::string &extra_message)
{
  // gather info to ask the user
  uint64_t amount = 0, amount_to_dests = 0, change = 0;
  size_t min_ring_size = ~0;
  std::unordered_map<cryptonote::account_public_address, std::pair<std::string, uint64_t>> dests;
  int first_known_non_zero_change_index = -1;
  for (size_t n = 0; n < get_num_txes(); ++n)
  {
    const tools::wallet2::tx_construction_data &cd = get_tx(n);

    for (size_t s = 0; s < cd.sources.size(); ++s)
    {
      amount += cd.sources[s].amount;
      size_t ring_size = cd.sources[s].outputs.size();
      if (ring_size < min_ring_size)
        min_ring_size = ring_size;
    }
    for (size_t d = 0; d < cd.splitted_dsts.size(); ++d)
    {
      const cryptonote::tx_destination_entry &entry = cd.splitted_dsts[d];
      std::string address, standard_address = get_account_address_as_str(m_wallet.testnet(), entry.is_subaddress, entry.addr);
      address = standard_address;
      auto i = dests.find(entry.addr);
      if (i == dests.end())
        dests.insert(std::make_pair(entry.addr, std::make_pair(address, entry.amount)));
      else
        i->second.second += entry.amount;
      amount_to_dests += entry.amount;
    }
    if (cd.change_dts.amount > 0)
    {
      auto it = dests.find(cd.change_dts.addr);
      if (it == dests.end())
      {
        m_status = Status_Error;
        m_errorString = tr("Claimed change does not go to a paid address");
        return false;
      }
      if (it->second.second < cd.change_dts.amount)
      {
        m_status = Status_Error;
        m_errorString = tr("Claimed change is larger than payment to the change address");
        return  false;
      }
      if (cd.change_dts.amount > 0)
      {
        if (first_known_non_zero_change_index == -1)
          first_known_non_zero_change_index = n;
        if (memcmp(&cd.change_dts.addr, &get_tx(first_known_non_zero_change_index).change_dts.addr, sizeof(cd.change_dts.addr)))
        {
          m_status = Status_Error;
          m_errorString = tr("Change goes to more than one address");
          return false;
        }
      }
      change += cd.change_dts.amount;
      it->second.second -= cd.change_dts.amount;
      if (it->second.second == 0)
        dests.erase(cd.change_dts.addr);
    }
  }
  std::string dest_string;
  for (auto i = dests.begin(); i != dests.end(); )
  {
    dest_string += (boost::format(tr("sending %s to %s")) % cryptonote::print_money(i->second.second) % i->second.first).str();
    ++i;
    if (i != dests.end())
      dest_string += ", ";
  }
  if (dest_string.empty())
    dest_string = tr("with no destinations");

  std::string change_string;
  if (change > 0)
  {
    std::string address = get_account_address_as_str(m_wallet.m_wallet->testnet(), get_tx(0).subaddr_account > 0, get_tx(0).change_dts.addr);
    change_string += (boost::format(tr("%s change to %s")) % cryptonote::print_money(change) % address).str();
  }
  else
    change_string += tr("no change");
  uint64_t fee = amount - amount_to_dests;
  m_confirmationMessage = (boost::format(tr("Loaded %lu transactions, for %s, fee %s, %s, %s, with min ring size %lu. %s")) % (unsigned long)get_num_txes() % cryptonote::print_money(amount) % cryptonote::print_money(fee) % dest_string % change_string % (unsigned long)min_ring_size % extra_message).str();
  return true;
}

std::vector<uint64_t> UnsignedTransactionImpl::amount() const
{
    std::vector<uint64_t> result;
    for (const auto &utx : m_unsigned_tx_set.txes)   {
        for (const auto &unsigned_dest : utx.dests) {
            result.push_back(unsigned_dest.amount);
        }
    }
    return result;
}

std::vector<uint64_t> UnsignedTransactionImpl::fee() const
{
    std::vector<uint64_t> result;
    for (const auto &utx : m_unsigned_tx_set.txes) {
        uint64_t fee = 0;
        for (const auto &i: utx.sources) fee += i.amount;
        for (const auto &i: utx.splitted_dsts) fee -= i.amount;
        result.push_back(fee);
    }   
    return result;
} 

std::vector<uint64_t> UnsignedTransactionImpl::mixin() const
{
    std::vector<uint64_t> result;    
    for (const auto &utx: m_unsigned_tx_set.txes) {
        size_t min_mixin = ~0;
        // TODO: Is this loop needed or is sources[0] ?
        for (size_t s = 0; s < utx.sources.size(); ++s) {
            size_t mixin = utx.sources[s].outputs.size() - 1;
                if (mixin < min_mixin)
                    min_mixin = mixin;
        }
        result.push_back(min_mixin);
    }
    return result;
}    

uint64_t UnsignedTransactionImpl::txCount() const
{
    return m_unsigned_tx_set.txes.size();
}

std::vector<std::string> UnsignedTransactionImpl::paymentId() const 
{
    std::vector<string> result;
    return result;
    for (const auto &utx: m_unsigned_tx_set.txes) {     
      result.push_back("");
    }
    return result;
}

std::vector<std::string> UnsignedTransactionImpl::recipientAddress() const 
{
    // TODO: return integrated address if short payment ID exists
    std::vector<string> result;
    for (const auto &utx: m_unsigned_tx_set.txes) {
        result.push_back(cryptonote::get_account_address_as_str(m_wallet.m_wallet->testnet(), utx.dests[0].is_subaddress, utx.dests[0].addr));
    }
    return result;
}

uint64_t UnsignedTransactionImpl::minMixinCount() const
{    
    uint64_t min_mixin = ~0;  
    for (const auto &utx: m_unsigned_tx_set.txes) {
        for (size_t s = 0; s < utx.sources.size(); ++s) {
            size_t mixin = utx.sources[s].outputs.size() - 1;
                if (mixin < min_mixin)
                    min_mixin = mixin;
        }
    }
    return min_mixin;
}

} // namespace



