package pubsub

import (
	"context"
	"encoding/json"
	"fmt"
	"log"

	"github.com/libp2pchannel/signaling-server/model"
	"github.com/redis/go-redis/v9"
)

const channelPrefix = "sig:events:"

// redisEvent is the wire format for events published via Redis.
type redisEvent struct {
	TargetPeerID string `json:"target_peer_id"`
	EventType    string `json:"event_type"`
	EventData    string `json:"event_data"`
}

// RedisPubSub delivers SSE events across signaling instances via Redis Pub/Sub.
// Each instance subscribes to its own channel: sig:events:{nodeID}.
// Publishing looks up the target node and publishes to its channel.
type RedisPubSub struct {
	client *redis.Client
	nodeID string
	sub    *redis.PubSub
	out    chan Event
	done   chan struct{}
}

func NewRedisPubSub(redisURL, nodeID string) (*RedisPubSub, error) {
	opts, err := redis.ParseURL(redisURL)
	if err != nil {
		return nil, fmt.Errorf("invalid redis URL: %w", err)
	}
	client := redis.NewClient(opts)

	ctx := context.Background()
	channel := channelPrefix + nodeID
	sub := client.Subscribe(ctx, channel)

	if _, err := sub.Receive(ctx); err != nil {
		return nil, fmt.Errorf("redis subscribe failed: %w", err)
	}

	rps := &RedisPubSub{
		client: client,
		nodeID: nodeID,
		sub:    sub,
		out:    make(chan Event, 256),
		done:   make(chan struct{}),
	}

	go rps.readLoop()

	log.Printf("[redis-pubsub] subscribed to channel %s", channel)
	return rps, nil
}

func (r *RedisPubSub) readLoop() {
	ch := r.sub.Channel()
	for {
		select {
		case msg, ok := <-ch:
			if !ok {
				return
			}
			var evt redisEvent
			if err := json.Unmarshal([]byte(msg.Payload), &evt); err != nil {
				log.Printf("[redis-pubsub] unmarshal error: %v", err)
				continue
			}
			r.out <- Event{
				TargetPeerID: evt.TargetPeerID,
				TargetNodeID: r.nodeID,
				SSEEvent: model.SSEEvent{
					Type: evt.EventType,
					Data: evt.EventData,
				},
			}
		case <-r.done:
			return
		}
	}
}

func (r *RedisPubSub) Publish(_ context.Context, evt Event) error {
	targetChannel := channelPrefix + evt.TargetNodeID
	data, err := json.Marshal(redisEvent{
		TargetPeerID: evt.TargetPeerID,
		EventType:    evt.SSEEvent.Type,
		EventData:    evt.SSEEvent.Data,
	})
	if err != nil {
		return err
	}
	return r.client.Publish(context.Background(), targetChannel, string(data)).Err()
}

func (r *RedisPubSub) Subscribe(_ context.Context) (<-chan Event, error) {
	return r.out, nil
}

func (r *RedisPubSub) Close() error {
	select {
	case <-r.done:
	default:
		close(r.done)
	}
	r.sub.Close()
	return r.client.Close()
}
