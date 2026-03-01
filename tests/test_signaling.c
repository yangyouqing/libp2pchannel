/*
 * Test the signaling client JSON serialization/deserialization.
 */

#include "p2p_signaling.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_json_roundtrip(void)
{
    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_ICE_CANDIDATE;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "peer-1");
    snprintf(msg.to_peer, sizeof(msg.to_peer), "peer-2");
    snprintf(msg.room_id, sizeof(msg.room_id), "room-test");
    snprintf(msg.candidate, sizeof(msg.candidate), "candidate:1 1 UDP 2122252543 192.168.1.1 50000 typ host");

    char json[P2P_SIG_MAX_MSG_SIZE];
    int ret = p2p_sig_message_to_json(&msg, json, sizeof(json));
    assert(ret == 0);
    printf("Serialized: %s\n", json);

    p2p_sig_message_t parsed;
    ret = p2p_sig_message_from_json(json, strlen(json), &parsed);
    assert(ret == 0);

    assert(parsed.type == P2P_SIG_MSG_ICE_CANDIDATE);
    assert(strcmp(parsed.from_peer, "peer-1") == 0);
    assert(strcmp(parsed.to_peer, "peer-2") == 0);
    assert(strcmp(parsed.room_id, "room-test") == 0);
    assert(strstr(parsed.candidate, "192.168.1.1") != NULL);

    printf("JSON roundtrip: PASSED\n");
}

static void test_turn_credentials_json(void)
{
    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_TURN_CREDENTIALS;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "server");
    snprintf(msg.to_peer, sizeof(msg.to_peer), "peer-1");
    snprintf(msg.turn_username, sizeof(msg.turn_username), "1709123456:peer-1");
    snprintf(msg.turn_password, sizeof(msg.turn_password), "abc123base64==");
    snprintf(msg.turn_server, sizeof(msg.turn_server), "turn.example.com");
    msg.turn_port = 3478;
    msg.turn_ttl = 86400;

    char json[P2P_SIG_MAX_MSG_SIZE];
    assert(p2p_sig_message_to_json(&msg, json, sizeof(json)) == 0);
    printf("TURN cred JSON: %s\n", json);

    p2p_sig_message_t parsed;
    assert(p2p_sig_message_from_json(json, strlen(json), &parsed) == 0);
    assert(parsed.type == P2P_SIG_MSG_TURN_CREDENTIALS);
    assert(parsed.turn_port == 3478);
    assert(parsed.turn_ttl == 86400);
    assert(strcmp(parsed.turn_server, "turn.example.com") == 0);

    printf("TURN credentials JSON: PASSED\n");
}

int main(void)
{
    printf("=== Signaling Protocol Tests ===\n\n");
    test_json_roundtrip();
    test_turn_credentials_json();
    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
