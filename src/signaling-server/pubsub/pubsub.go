package pubsub

import (
	"context"

	"github.com/libp2pchannel/signaling-server/model"
)

type Event struct {
	TargetPeerID string         `json:"target_peer_id"`
	TargetNodeID string         `json:"target_node_id"`
	SSEEvent     model.SSEEvent `json:"sse_event"`
}

type PubSub interface {
	Publish(ctx context.Context, evt Event) error
	Subscribe(ctx context.Context) (<-chan Event, error)
	RegisterPeer(peerID string, ch chan model.SSEEvent)
	UnregisterPeer(peerID string)
	Close() error
}
