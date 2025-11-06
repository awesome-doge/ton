#!/usr/bin/env bash
# Example: Run TON Lite Client connected to Testnet
# Description:
#   This script shows how to connect to TON testnet using lite-client.
#   It automatically downloads global config and starts the client.

set -e

CONFIG_URL="https://ton-blockchain.github.io/testnet-global.config.json"
CONFIG_FILE="testnet-global.config.json"

echo "[INFO] Downloading TON Testnet config..."
curl -s -o "$CONFIG_FILE" "$CONFIG_URL"

echo "[INFO] Starting TON Lite Client..."
./lite-client/lite-client \
  -C "$CONFIG_FILE" \
  -v 2 \
  -p 1

# Example usage inside client:
# > getaccount <account_address>
# > last
# > quit
