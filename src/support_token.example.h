/*
 * support_token.example.h  --  copy to support_token.h (gitignored) and fill in.
 *
 *   cp src/support_token.example.h src/support_token.h
 *
 * The support-bundle auth token. It ships inside the firmware .bin, so treat it
 * as a shared secret, not a strong credential: the VPS (fleet-monitor/app/
 * server.py) caps bundles per-MAC and globally and validates the MAC format, so
 * a leaked token can't grow the store without bound. Rotate here + on the VPS
 * together, then redeploy. A checkout without support_token.h still builds; its
 * "Send diagnostics" simply won't authenticate.
 */
#pragma once

#define SUPPORT_TOKEN "put-the-real-support-token-here"
