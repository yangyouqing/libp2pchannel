package pubsub

import (
	"context"
	"log"
	"sync"

	"github.com/libp2pchannel/signaling-server/model"
)

// LocalPubSub is a single-process pub/sub that delivers events directly
// to local SSE peer channels. Used when REDIS_URL is not configured.
type LocalPubSub struct {
	peers map[string]chan model.SSEEvent
	out   chan Event
	mu    sync.RWMutex
	done  chan struct{}
}

func NewLocalPubSub() *LocalPubSub {
	return &LocalPubSub{
		peers: make(map[string]chan model.SSEEvent),
		out:   make(chan Event, 256),
		done:  make(chan struct{}),
	}
}

// RegisterPeer adds a local SSE peer channel.
func (l *LocalPubSub) RegisterPeer(peerID string, ch chan model.SSEEvent) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.peers[peerID] = ch
}

// UnregisterPeer removes a local SSE peer channel.
func (l *LocalPubSub) UnregisterPeer(peerID string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	delete(l.peers, peerID)
}

func (l *LocalPubSub) Publish(_ context.Context, evt Event) error {
	l.mu.RLock()
	ch, ok := l.peers[evt.TargetPeerID]
	l.mu.RUnlock()

	if !ok {
		log.Printf("[pubsub-local] peer %s not found locally, dropping event %s",
			evt.TargetPeerID, evt.SSEEvent.Type)
		return nil
	}

	select {
	case ch <- evt.SSEEvent:
	default:
		log.Printf("[pubsub-local] event channel full for peer %s, dropping %s",
			evt.TargetPeerID, evt.SSEEvent.Type)
	}
	return nil
}

func (l *LocalPubSub) Subscribe(_ context.Context) (<-chan Event, error) {
	return l.out, nil
}

func (l *LocalPubSub) Close() error {
	select {
	case <-l.done:
	default:
		close(l.done)
	}
	return nil
}
