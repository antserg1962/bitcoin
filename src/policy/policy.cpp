// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// NOTE: This file is intended to be customised by the end user, and includes only local node policy logic

#include "policy/policy.h"

#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "validation.h"
#include "coins.h"
#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"


CAmount GetDustThreshold(const CTxOut& txout, const CFeeRate& dustRelayFeeIn)
{
    // "Dust" is defined in terms of dustRelayFee,
    // which has units satoshis-per-kilobyte.
    // If you'd pay more in fees than the value of the output
    // to spend something, then we consider it dust.
    // A typical spendable non-segwit txout is 34 bytes big, and will
    // need a CTxIn of at least 148 bytes to spend:
    // so dust is a spendable txout less than
    // 182*dustRelayFee/1000 (in satoshis).
    // 546 satoshis at the default rate of 3000 sat/kB.
    // A typical spendable segwit txout is 31 bytes big, and will
    // need a CTxIn of at least 67 bytes to spend:
    // so dust is a spendable txout less than
    // 98*dustRelayFee/1000 (in satoshis).
    // 294 satoshis at the default rate of 3000 sat/kB.
    if (txout.scriptPubKey.IsUnspendable())
        return 0;

    size_t nSize = GetSerializeSize(txout, SER_DISK, 0);
    int witnessversion = 0;
    std::vector<unsigned char> witnessprogram;

    if (txout.scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
        // sum the sizes of the parts of a transaction input
        // with 75% segwit discount applied to the script size.
        nSize += (32 + 4 + 1 + (107 / WITNESS_SCALE_FACTOR) + 4);
    } else {
        nSize += (32 + 4 + 1 + 107 + 4); // the 148 mentioned above
    }

    return dustRelayFeeIn.GetFee(nSize);
}

bool IsDust(const CTxOut& txout, const CFeeRate& dustRelayFeeIn)
{
    return (txout.nValue < GetDustThreshold(txout, dustRelayFeeIn));
}

    /**
     * Check transaction inputs to mitigate two
     * potential denial-of-service attacks:
     * 
     * 1. scriptSigs with extra data stuffed into them,
     *    not consumed by scriptPubKey (or P2SH script)
     * 2. P2SH scripts with a crazy number of expensive
     *    CHECKSIG/CHECKMULTISIG operations
     *
     * Why bother? To avoid denial-of-service attacks; an attacker
     * can submit a standard HASH... OP_EQUAL transaction,
     * which will get accepted into blocks. The redemption
     * script can be anything; an attacker could use a very
     * expensive-to-check-upon-redemption script like:
     *   DUP CHECKSIG DROP ... repeated 100 times... OP_1
     *
     * Note this must assign whichType even if returning false, in case
     * IsStandardTx ignores the "scriptpubkey" rejection.
     */

bool IsStandard(const CScript& scriptPubKey, txnouttype& whichType, const bool witnessEnabled)
{
    std::vector<std::vector<unsigned char> > vSolutions;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_MULTISIG)
    {
        unsigned char m = vSolutions.front()[0];
        unsigned char n = vSolutions.back()[0];
        // Support up to x-of-3 multisig txns as standard
        if (n < 1 || n > 3)
            return false;
        if (m < 1 || m > n)
            return false;
    } else if (whichType == TX_NULL_DATA &&
               (!fAcceptDatacarrier || scriptPubKey.size() > nMaxDatacarrierBytes))
          return false;

    else if (!witnessEnabled && (whichType == TX_WITNESS_V0_KEYHASH || whichType == TX_WITNESS_V0_SCRIPTHASH))
        return false;

    return whichType != TX_NONSTANDARD;
}

namespace {
    inline bool MaybeReject_(std::string& out_reason, const std::string& reason, const std::string& reason_prefix, const ignore_rejects_type& ignore_rejects) {
        if (ignore_rejects.count(reason_prefix + reason)) {
            return false;
        }

        out_reason = reason_prefix + reason;
        return true;
    }
}

#define MaybeReject(reason)  do {  \
    if (MaybeReject_(out_reason, reason, reason_prefix, ignore_rejects)) {  \
        return false;  \
    }  \
} while(0)

bool IsStandardTx(const CTransaction& tx, std::string& out_reason, const bool witnessEnabled, const ignore_rejects_type& ignore_rejects)
{
    const std::string reason_prefix;

    if (tx.nVersion > CTransaction::MAX_STANDARD_VERSION || tx.nVersion < 1) {
        MaybeReject("version");
    }

    if (!ignore_rejects.count("tx-size")) {
        // Extremely large transactions with lots of inputs can cost the network
        // almost as much to process as they cost the sender in fees, because
        // computing signature hashes is O(ninputs*txsize). Limiting transactions
        // to MAX_STANDARD_TX_WEIGHT mitigates CPU exhaustion attacks.
        unsigned int sz = GetTransactionWeight(tx);
        if (sz >= MAX_STANDARD_TX_WEIGHT) {
            out_reason = "tx-size";
            return false;
        }
    }

    bool fCheckPushOnly = !ignore_rejects.count("scriptsig-not-pushonly");
    if ((!ignore_rejects.count("scriptsig-size")) || fCheckPushOnly) {
        for (const CTxIn& txin : tx.vin)
        {
            // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
            // keys (remember the 520 byte limit on redeemScript size). That works
            // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
            // bytes of scriptSig, which we round off to 1650 bytes for some minor
            // future-proofing. That's also enough to spend a 20-of-20
            // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
            // considered standard.
            if (txin.scriptSig.size() > 1650) {
                MaybeReject("scriptsig-size");
            }
            if (fCheckPushOnly && !txin.scriptSig.IsPushOnly()) {
                out_reason = "scriptsig-not-pushonly";
                return false;
            }
        }
    }

    if (!(ignore_rejects.count("scriptpubkey") && ignore_rejects.count("bare-multisig") && ignore_rejects.count("dust") && ignore_rejects.count("multi-op-return"))) {
        unsigned int nDataOut = 0;
        txnouttype whichType;
        for (const CTxOut& txout : tx.vout) {
            if (!::IsStandard(txout.scriptPubKey, whichType, witnessEnabled)) {
                MaybeReject("scriptpubkey");
            }

            if (whichType == TX_NULL_DATA)
                nDataOut++;
            else {
                if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd)) {
                    MaybeReject("bare-multisig");
                }
                if (IsDust(txout, ::dustRelayFee)) {
                    MaybeReject("dust");
                }
            }
        }

        // only one OP_RETURN txout is permitted
        if (nDataOut > 1) {
            MaybeReject("multi-op-return");
        }
    }

    return true;
}

bool AreInputsStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs, const std::string& reason_prefix, std::string& out_reason, const ignore_rejects_type& ignore_rejects)
{
    if (tx.IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut& prev = mapInputs.AccessCoin(tx.vin[i].prevout).out;

        std::vector<std::vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions)) {
            MaybeReject("script-unknown");
        }

        if (whichType == TX_SCRIPTHASH)
        {
            if (!tx.vin[i].scriptSig.IsPushOnly()) {
                // The only way we got this far, is if the user ignored scriptsig-not-pushonly.
                // However, this case is invalid, and will be caught later on.
                // But for now, we don't want to run the [possibly expensive] script here.
                continue;
            }
            std::vector<std::vector<unsigned char> > stack;
            // convert the scriptSig into a stack, so we can inspect the redeemScript
            if (!EvalScript(ScriptExecution::Context::Sig, stack, tx.vin[i].scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SIGVERSION_BASE)) {
                // This case is also invalid or a bug
                out_reason = reason_prefix + "scriptsig-failure";
                return false;
            }
            if (stack.empty()) {
                // Also invalid
                out_reason = reason_prefix + "scriptcheck-missing";
                return false;
            }
            CScript subscript(stack.back().begin(), stack.back().end());
            if (subscript.GetSigOpCount(true) > MAX_P2SH_SIGOPS) {
                MaybeReject("scriptcheck-sigops");
            }
        }
    }

    return true;
}

bool IsWitnessStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs, const std::string& reason_prefix, std::string& out_reason, const ignore_rejects_type& ignore_rejects)
{
    if (tx.IsCoinBase())
        return true; // Coinbases are skipped

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        // We don't care if witness for this input is empty, since it must not be bloated.
        // If the script is invalid without witness, it would be caught sooner or later during validation.
        if (tx.vin[i].scriptWitness.IsNull())
            continue;

        const CTxOut &prev = mapInputs.AccessCoin(tx.vin[i].prevout).out;

        // get the scriptPubKey corresponding to this input:
        CScript prevScript = prev.scriptPubKey;

        if (prevScript.IsPayToScriptHash()) {
            std::vector <std::vector<unsigned char> > stack;
            // If the scriptPubKey is P2SH, we try to extract the redeemScript casually by converting the scriptSig
            // into a stack. We do not check IsPushOnly nor compare the hash as these will be done later anyway.
            // If the check fails at this stage, we know that this txid must be a bad one.
            if (!EvalScript(ScriptExecution::Context::Sig, stack, tx.vin[i].scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SIGVERSION_BASE)) {
                out_reason = reason_prefix + "scriptsig-failure";
                return false;
            }
            if (stack.empty())
            {
                out_reason = reason_prefix + "scriptcheck-missing";
                return false;
            }
            prevScript = CScript(stack.back().begin(), stack.back().end());
        }

        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;

        // Non-witness program must not be associated with any witness
        if (!prevScript.IsWitnessProgram(witnessversion, witnessprogram))
        {
            out_reason = reason_prefix + "nonwitness-input";
            return false;
        }

        // Check P2WSH standard limits
        if (witnessversion == 0 && witnessprogram.size() == 32) {
            if (tx.vin[i].scriptWitness.stack.back().size() > MAX_STANDARD_P2WSH_SCRIPT_SIZE)
            {
                MaybeReject("script-size");
            }
            size_t sizeWitnessStack = tx.vin[i].scriptWitness.stack.size() - 1;
            if (sizeWitnessStack > MAX_STANDARD_P2WSH_STACK_ITEMS)
            {
                MaybeReject("stackitem-count");
            }
            for (unsigned int j = 0; j < sizeWitnessStack; j++) {
                if (tx.vin[i].scriptWitness.stack[j].size() > MAX_STANDARD_P2WSH_STACK_ITEM_SIZE)
                {
                    MaybeReject("stackitem-size");
                }
            }
        }
    }
    return true;
}

CFeeRate incrementalRelayFee = CFeeRate(DEFAULT_INCREMENTAL_RELAY_FEE);
CFeeRate dustRelayFee = CFeeRate(DUST_RELAY_TX_FEE);
unsigned int nBytesPerSigOp = DEFAULT_BYTES_PER_SIGOP;
unsigned int nBytesPerSigOpStrict = DEFAULT_BYTES_PER_SIGOP_STRICT;

int64_t GetVirtualTransactionSize(int64_t nWeight, int64_t nSigOpCost)
{
    return (std::max(nWeight, nSigOpCost * nBytesPerSigOp) + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;
}

int64_t GetVirtualTransactionSize(const CTransaction& tx, int64_t nSigOpCost)
{
    return GetVirtualTransactionSize(GetTransactionWeight(tx), nSigOpCost);
}

int64_t GetAccurateTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, int flags)
{
    if (tx.IsCoinBase()) {
        return 0;
    }

    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin) {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs);
    }

    nSigOps *= WITNESS_SCALE_FACTOR;

    if (flags & SCRIPT_VERIFY_WITNESS) {
        for (const auto& txin : tx.vin) {
            const Coin& coin = inputs.AccessCoin(txin.prevout);
            assert(!coin.IsSpent());
            const CTxOut &prevout = coin.out;
            nSigOps += CountWitnessSigOps(txin.scriptSig, prevout.scriptPubKey, &txin.scriptWitness, flags);
        }
    }

    return nSigOps;
}
