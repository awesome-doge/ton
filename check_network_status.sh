#!/bin/bash
echo "Checking TON network connectivity..."
ping -c 3 ton.org && echo "Network OK" || echo "Network issue detected"
