package store

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"time"

	"github.com/redis/go-redis/v9"
)

const (
	peerKeyPrefix  = "sig:peer:"
	roomKeyPrefix  = "sig:room:"
	peersSetKey    = "sig:peers"
	roomsSetKey    = "sig:rooms"
	peerTTL        = 24 * time.Hour
	roomTTL        = 24 * time.Hour
)

type redisPeerData struct {
	ID          string `json:"id"`
	RoomID      string `json:"room_id"`
	IsPublisher bool   `json:"is_publisher"`
	NodeID      string `json:"node_id"`
	OnlineAt    int64  `json:"online_at"`
}

type redisRoomData struct {
	ID             string   `json:"id"`
	PublisherID    string   `json:"publisher_id"`
	SubscriberIDs  []string `json:"subscriber_ids"`
	MaxSubscribers int      `json:"max_subscribers"`
	CachedOffer    *CachedICE `json:"cached_offer,omitempty"`
}

type RedisStore struct {
	client         *redis.Client
	nodeID         string
	maxSubscribers int
}

func NewRedisStore(redisURL, nodeID string, maxSubscribers int) (*RedisStore, error) {
	opts, err := redis.ParseURL(redisURL)
	if err != nil {
		return nil, fmt.Errorf("invalid redis URL: %w", err)
	}
	client := redis.NewClient(opts)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := client.Ping(ctx).Err(); err != nil {
		return nil, fmt.Errorf("redis ping failed: %w", err)
	}

	log.Printf("[redis-store] connected to Redis (node %s)", nodeID)
	return &RedisStore{
		client:         client,
		nodeID:         nodeID,
		maxSubscribers: maxSubscribers,
	}, nil
}

func (r *RedisStore) Ping(ctx context.Context) error {
	return r.client.Ping(ctx).Err()
}

func (r *RedisStore) RegisterPeer(peerID, nodeID string) error {
	ctx := context.Background()
	data := redisPeerData{
		ID:       peerID,
		NodeID:   nodeID,
		OnlineAt: time.Now().Unix(),
	}

	existing, _ := r.getPeerData(ctx, peerID)
	if existing != nil {
		data.RoomID = existing.RoomID
		data.IsPublisher = existing.IsPublisher
	}

	return r.setPeerData(ctx, peerID, &data)
}

func (r *RedisStore) UnregisterPeer(peerID string) (*PeerMeta, error) {
	ctx := context.Background()
	data, err := r.getPeerData(ctx, peerID)
	if err != nil || data == nil {
		return nil, err
	}

	meta := dataToPeerMeta(data)

	if data.RoomID != "" {
		roomData, _ := r.getRoomData(ctx, data.RoomID)
		if roomData != nil {
			if data.IsPublisher {
				r.deleteRoom(ctx, data.RoomID)
			} else {
				newSubs := make([]string, 0)
				for _, id := range roomData.SubscriberIDs {
					if id != peerID {
						newSubs = append(newSubs, id)
					}
				}
				roomData.SubscriberIDs = newSubs
				r.setRoomData(ctx, data.RoomID, roomData)
			}
		}
	}

	r.client.Del(ctx, peerKeyPrefix+peerID)
	r.client.SRem(ctx, peersSetKey, peerID)
	log.Printf("[redis-store] peer %s unregistered", peerID)
	return meta, nil
}

func (r *RedisStore) GetPeer(peerID string) (*PeerMeta, error) {
	data, err := r.getPeerData(context.Background(), peerID)
	if err != nil || data == nil {
		return nil, err
	}
	return dataToPeerMeta(data), nil
}

func (r *RedisStore) EnsurePeer(peerID, nodeID string) (*PeerMeta, error) {
	ctx := context.Background()
	data, _ := r.getPeerData(ctx, peerID)
	if data != nil {
		return dataToPeerMeta(data), nil
	}

	data = &redisPeerData{
		ID:       peerID,
		NodeID:   nodeID,
		OnlineAt: time.Now().Unix(),
	}
	if err := r.setPeerData(ctx, peerID, data); err != nil {
		return nil, err
	}
	return dataToPeerMeta(data), nil
}

func (r *RedisStore) CreateRoom(roomID, publisherID string, maxSubs int) error {
	ctx := context.Background()

	existing, _ := r.getRoomData(ctx, roomID)
	if existing != nil {
		return fmt.Errorf("room %s already exists", roomID)
	}

	roomData := &redisRoomData{
		ID:             roomID,
		PublisherID:    publisherID,
		SubscriberIDs:  []string{},
		MaxSubscribers: maxSubs,
	}
	if err := r.setRoomData(ctx, roomID, roomData); err != nil {
		return err
	}

	peerData, _ := r.getPeerData(ctx, publisherID)
	if peerData != nil {
		peerData.RoomID = roomID
		peerData.IsPublisher = true
		r.setPeerData(ctx, publisherID, peerData)
	}

	log.Printf("[redis-store] created room %s by publisher %s", roomID, publisherID)
	return nil
}

func (r *RedisStore) JoinRoom(roomID, subscriberID string) (*RoomMeta, error) {
	ctx := context.Background()

	roomData, err := r.getRoomData(ctx, roomID)
	if err != nil {
		return nil, err
	}
	if roomData == nil {
		return nil, fmt.Errorf("room %s not found", roomID)
	}

	if len(roomData.SubscriberIDs) >= roomData.MaxSubscribers {
		return nil, fmt.Errorf("room %s is full (%d/%d)", roomID,
			len(roomData.SubscriberIDs), roomData.MaxSubscribers)
	}

	roomData.SubscriberIDs = append(roomData.SubscriberIDs, subscriberID)
	if err := r.setRoomData(ctx, roomID, roomData); err != nil {
		return nil, err
	}

	peerData, _ := r.getPeerData(ctx, subscriberID)
	if peerData != nil {
		peerData.RoomID = roomID
		peerData.IsPublisher = false
		r.setPeerData(ctx, subscriberID, peerData)
	}

	log.Printf("[redis-store] subscriber %s joined room %s (%d/%d)",
		subscriberID, roomID, len(roomData.SubscriberIDs), roomData.MaxSubscribers)

	return dataToRoomMeta(roomData), nil
}

func (r *RedisStore) LeaveRoom(peerID string) error {
	ctx := context.Background()

	peerData, _ := r.getPeerData(ctx, peerID)
	if peerData == nil || peerData.RoomID == "" {
		return nil
	}

	roomData, _ := r.getRoomData(ctx, peerData.RoomID)
	if roomData != nil {
		if peerData.IsPublisher {
			r.deleteRoom(ctx, peerData.RoomID)
		} else {
			newSubs := make([]string, 0)
			for _, id := range roomData.SubscriberIDs {
				if id != peerID {
					newSubs = append(newSubs, id)
				}
			}
			roomData.SubscriberIDs = newSubs
			r.setRoomData(ctx, peerData.RoomID, roomData)
		}
	}

	peerData.RoomID = ""
	peerData.IsPublisher = false
	r.setPeerData(ctx, peerID, peerData)
	return nil
}

func (r *RedisStore) GetRoom(roomID string) (*RoomMeta, error) {
	data, err := r.getRoomData(context.Background(), roomID)
	if err != nil || data == nil {
		return nil, err
	}
	return dataToRoomMeta(data), nil
}

func (r *RedisStore) CacheOffer(roomID string, offer *CachedICE) error {
	ctx := context.Background()
	data, err := r.getRoomData(ctx, roomID)
	if err != nil {
		return err
	}
	if data == nil {
		return fmt.Errorf("room %s not found", roomID)
	}
	data.CachedOffer = offer
	return r.setRoomData(ctx, roomID, data)
}

func (r *RedisStore) GetCachedOffer(roomID string) (*CachedICE, error) {
	data, err := r.getRoomData(context.Background(), roomID)
	if err != nil || data == nil {
		return nil, err
	}
	return data.CachedOffer, nil
}

func (r *RedisStore) ListPeers() ([]PeerMeta, error) {
	ctx := context.Background()
	ids, err := r.client.SMembers(ctx, peersSetKey).Result()
	if err != nil {
		return nil, err
	}

	result := make([]PeerMeta, 0, len(ids))
	for _, id := range ids {
		data, _ := r.getPeerData(ctx, id)
		if data != nil {
			result = append(result, *dataToPeerMeta(data))
		}
	}
	return result, nil
}

func (r *RedisStore) ListRooms() ([]RoomMeta, error) {
	ctx := context.Background()
	ids, err := r.client.SMembers(ctx, roomsSetKey).Result()
	if err != nil {
		return nil, err
	}

	result := make([]RoomMeta, 0, len(ids))
	for _, id := range ids {
		data, _ := r.getRoomData(ctx, id)
		if data != nil {
			result = append(result, *dataToRoomMeta(data))
		}
	}
	return result, nil
}

func (r *RedisStore) getPeerData(ctx context.Context, peerID string) (*redisPeerData, error) {
	val, err := r.client.Get(ctx, peerKeyPrefix+peerID).Result()
	if err == redis.Nil {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var data redisPeerData
	if err := json.Unmarshal([]byte(val), &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (r *RedisStore) setPeerData(ctx context.Context, peerID string, data *redisPeerData) error {
	b, err := json.Marshal(data)
	if err != nil {
		return err
	}
	pipe := r.client.Pipeline()
	pipe.Set(ctx, peerKeyPrefix+peerID, string(b), peerTTL)
	pipe.SAdd(ctx, peersSetKey, peerID)
	_, err = pipe.Exec(ctx)
	return err
}

func (r *RedisStore) getRoomData(ctx context.Context, roomID string) (*redisRoomData, error) {
	val, err := r.client.Get(ctx, roomKeyPrefix+roomID).Result()
	if err == redis.Nil {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var data redisRoomData
	if err := json.Unmarshal([]byte(val), &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (r *RedisStore) setRoomData(ctx context.Context, roomID string, data *redisRoomData) error {
	b, err := json.Marshal(data)
	if err != nil {
		return err
	}
	pipe := r.client.Pipeline()
	pipe.Set(ctx, roomKeyPrefix+roomID, string(b), roomTTL)
	pipe.SAdd(ctx, roomsSetKey, roomID)
	_, err = pipe.Exec(ctx)
	return err
}

func (r *RedisStore) deleteRoom(ctx context.Context, roomID string) {
	r.client.Del(ctx, roomKeyPrefix+roomID)
	r.client.SRem(ctx, roomsSetKey, roomID)
	log.Printf("[redis-store] room %s deleted", roomID)
}

func dataToPeerMeta(d *redisPeerData) *PeerMeta {
	return &PeerMeta{
		ID:          d.ID,
		RoomID:      d.RoomID,
		IsPublisher: d.IsPublisher,
		NodeID:      d.NodeID,
		OnlineAt:    time.Unix(d.OnlineAt, 0),
	}
}

func dataToRoomMeta(d *redisRoomData) *RoomMeta {
	subs := d.SubscriberIDs
	if subs == nil {
		subs = []string{}
	}
	return &RoomMeta{
		ID:             d.ID,
		PublisherID:    d.PublisherID,
		SubscriberIDs:  subs,
		MaxSubscribers: d.MaxSubscribers,
		CachedOffer:    d.CachedOffer,
	}
}
