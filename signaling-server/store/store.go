package store

import "time"

type CachedICE struct {
	From       string   `json:"from"`
	SDP        string   `json:"sdp"`
	Candidates []string `json:"candidates"`
}

type PeerMeta struct {
	ID          string
	RoomID      string
	IsPublisher bool
	NodeID      string
	OnlineAt    time.Time
}

type RoomMeta struct {
	ID             string
	PublisherID    string
	SubscriberIDs  []string
	MaxSubscribers int
	CachedOffer    *CachedICE
}

type Store interface {
	RegisterPeer(peerID, nodeID string) error
	UnregisterPeer(peerID string) (*PeerMeta, error)
	GetPeer(peerID string) (*PeerMeta, error)
	EnsurePeer(peerID, nodeID string) (*PeerMeta, error)

	CreateRoom(roomID, publisherID string, maxSubs int) error
	JoinRoom(roomID, subscriberID string) (*RoomMeta, error)
	LeaveRoom(peerID string) error
	GetRoom(roomID string) (*RoomMeta, error)

	CacheOffer(roomID string, offer *CachedICE) error
	GetCachedOffer(roomID string) (*CachedICE, error)

	ListPeers() ([]PeerMeta, error)
	ListRooms() ([]RoomMeta, error)
}
