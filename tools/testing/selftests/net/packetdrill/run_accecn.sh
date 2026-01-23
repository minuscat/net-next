#!/bin/bash
#set -x
IFS=$'\n\t'

# Location of packetdrill directory (adjust if needed)
PACKETDRILL="/home/anc/packetdrill_neal/gtests/net/packetdrill"
export PATH="$PACKETDRILL:$PATH"

# Path to ksft_runner.sh
KSFT_RUNNER="./ksft_runner.sh"

# Directory where ACCECN .pkt files are located
TEST_DIR="./"

# Optional filter (glob pattern), default "*"
FILTER="${1:-*}"

# List of test files
file_name=(
    "tcp_accecn_2nd_data_as_first_connect.pkt"
    "tcp_accecn_2nd_data_as_first.pkt"
    "tcp_accecn_3rd_ack_after_synack_rxmt.pkt"
    "tcp_accecn_3rd_ack_ce_updates_received_ce.pkt"
    "tcp_accecn_3rd_ack_lost_data_ce.pkt"
    "tcp_accecn_3rd_dups.pkt"
    "tcp_accecn_acc_ecn_disabled.pkt"
    "tcp_accecn_accecn_then_notecn_syn.pkt"
    "tcp_accecn_accecn_to_rfc3168.pkt"
    "tcp_accecn_client_accecn_options_drop.pkt"
    "tcp_accecn_client_accecn_options_lost.pkt"
    "tcp_accecn_clientside_disabled.pkt"
    "tcp_accecn_close_local_close_then_remote_fin.pkt"
    "tcp_accecn_delivered_2ndlargeack.pkt"
    "tcp_accecn_delivered_falseoverflow_detect.pkt"
    "tcp_accecn_delivered_largeack2.pkt"
    "tcp_accecn_delivered_largeack.pkt"
    "tcp_accecn_delivered_maxack.pkt"
    "tcp_accecn_delivered_updates.pkt"
    "tcp_accecn_ecn3.pkt"
    "tcp_accecn_ecn_field_updates_opt.pkt"
    "tcp_accecn_ipflags_drop.pkt"
    "tcp_accecn_listen_opt_drop.pkt"
    "tcp_accecn_multiple_syn_ack_drop.pkt"
    "tcp_accecn_multiple_syn_drop.pkt"
    "tcp_accecn_negotiation_bleach.pkt"
    "tcp_accecn_negotiation_connect.pkt"
    "tcp_accecn_negotiation_listen.pkt"
    "tcp_accecn_negotiation_noopt_connect.pkt"
    "tcp_accecn_negotiation_optenable.pkt"
    "tcp_accecn_no_ecn_after_accecn.pkt"
    "tcp_accecn_noopt.pkt"
    "tcp_accecn_noprogress.pkt"
    "tcp_accecn_notecn_then_accecn_syn.pkt"
    "tcp_accecn_rfc3168_to_fallback.pkt"
    "tcp_accecn_rfc3168_to_rfc3168.pkt"
    "tcp_accecn_sack_space_grab.pkt"
    "tcp_accecn_sack_space_grab_with_ts.pkt"
    "tcp_accecn_serverside_accecn_disabled1.pkt"
    "tcp_accecn_serverside_accecn_disabled2.pkt"
    "tcp_accecn_serverside_broken.pkt"
    "tcp_accecn_serverside_ecn_disabled.pkt"
    "tcp_accecn_serverside_only.pkt"
    "tcp_accecn_syn_ace_flags_acked_after_retransmit.pkt"
    "tcp_accecn_syn_ace_flags_drop.pkt"
    "tcp_accecn_syn_ack_ace_flags_acked_after_retransmit.pkt"
    "tcp_accecn_syn_ack_ace_flags_drop.pkt"
    "tcp_accecn_synack_ce.pkt"
    "tcp_accecn_synack_ce_updates_delivered_ce.pkt"
    "tcp_accecn_synack_ect0.pkt"
    "tcp_accecn_synack_ect1.pkt"
    "tcp_accecn_synack_rexmit.pkt"
    "tcp_accecn_synack_rxmt.pkt"
    "tcp_accecn_syn_ce.pkt"
    "tcp_accecn_syn_ect0.pkt"
    "tcp_accecn_syn_ect1.pkt"
    "tcp_accecn_tsnoprogress.pkt"
    "tcp_accecn_tsprogress.pkt"
)

if [[ $# -gt 1 ]]; then
    echo "Usage: $0 [filter]" >&2
    exit 2
fi

if [[ ! -d "$TEST_DIR" ]]; then
    echo "Directory $TEST_DIR not found" >&2
    exit 2
fi

if [[ ! -x "$KSFT_RUNNER" ]]; then
    if [[ -f "$KSFT_RUNNER" ]]; then
        echo "Runner $KSFT_RUNNER exists but is not executable" >&2
    else
        echo "Runner $KSFT_RUNNER not found" >&2
    fi
    exit 2
fi

declare -a file_run=()
cases=0
for file in "${file_name[@]}"; do
    if [[ -s "$TEST_DIR/$file" ]]; then
        case "$file" in
            $FILTER)
                file_run+=("$file")
                ((cases++))
                echo "Add case $cases: $TEST_DIR/$file for testing"
                ;;
            *)
                ;;
        esac
    fi
done

if [[ ${#file_run[@]} -eq 0 ]]; then
    echo "No .pkt files match filter criteria: $FILTER" >&2
    exit 3
fi

pass=0
fail=0
for file in "${file_run[@]}"; do
    if command -v sudo >/dev/null 2>&1; then
        sudo -E env PATH="$PATH" "$KSFT_RUNNER" "$TEST_DIR/$file"
    else
        env PATH="$PATH" "$KSFT_RUNNER" "$TEST_DIR/$file"
    fi
    rc=$?
    if [[ $rc -eq 0 ]]; then
        ((pass++))
        echo "[PASS] $file"
    else
        ((fail++))
        echo "[FAIL] $file (exit code: $rc)"
    fi
    echo "-----------------------------------"
done

echo "Total: $cases cases, $pass passed, $fail failed."
