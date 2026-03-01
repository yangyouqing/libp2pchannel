package room

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"time"

	"github.com/libp2pchannel/signaling-server/model"
)

type Peer struct {
	ID       string
	Conn     net.Conn
	RoomID   string
	IsPublisher bool
	LastPing time.Time
	mu       sync.Mutex
}

func (p *Peer) Send(msg *model.SignalMessage) error {
	data, err := json.Marshal(msg)
	if err != nil {
		return err
	}
	p.mu.Lock()
	defer p.mu.Unlock()

	// length-prefixed frame: [4-byte big-endian length][JSON]
	lenBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(lenBuf, uint32(len(data)))
	if _, err := p.Conn.Write(lenBuf); err != nil {
		return err
	}
	_, err = p.Conn.Write(data)
	return err
}

type Room struct {
	ID            string
	Publisher     *Peer
	Subscribers   map[string]*Peer
	MaxSubscribers int
	mu            sync.RWMutex
}

func NewRoom(id string, publisher *Peer, maxSubs int) *Room {
	return &Room{
		ID:             id,
		Publisher:       publisher,
		Subscribers:    make(map[string]*Peer),
		MaxSubscribers: maxSubs,
	}
}

func (r *Room) AddSubscriber(peer *Peer) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.Subscribers) >= r.MaxSubscribers {
		return fmt.Errorf("room %s is full (%d/%d)", r.ID, len(r.Subscribers), r.MaxSubscribers)
	}
	r.Subscribers[peer.ID] = peer
	return nil
}

func (r *Room) RemoveSubscriber(peerID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.Subscribers, peerID)
}

func (r *Room) GetSubscriber(peerID string) *Peer {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.Subscribers[peerID]
}

func (r *Room) SubscriberCount() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.Subscribers)
}

// Manager manages all rooms and peers.
type Manager struct {
	rooms          map[string]*Room
	peers          map[string]*Peer // peerID -> Peer
	maxSubscribers int
	mu             sync.RWMutex
}

func NewManager(maxSubscribers int) *Manager {
	return &Manager{
		rooms:          make(map[string]*Room),
		peers:          make(map[string]*Peer),
		maxSubscribers: maxSubscribers,
	}
}

func (m *Manager) RegisterPeer(peer *Peer) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.peers[peer.ID] = peer
}

func (m *Manager) UnregisterPeer(peerID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	peer, ok := m.peers[peerID]
	if !ok {
		return
	}

	if peer.RoomID != "" {
		if room, exists := m.rooms[peer.RoomID]; exists {
			if peer.IsPublisher {
				// Notify all subscribers that room is closing
				room.mu.RLock()
				for _, sub := range room.Subscribers {
					sub.Send(&model.SignalMessage{
						Type: model.MsgPeerLeft,
						From: peerID,
						Room: peer.RoomID,
					})
				}
				room.mu.RUnlock()
				delete(m.rooms, peer.RoomID)
			} else {
				room.RemoveSubscriber(peerID)
				if room.Publisher != nil {
					room.Publisher.Send(&model.SignalMessage{
						Type: model.MsgPeerLeft,
						From: peerID,
						Room: peer.RoomID,
					})
				}
			}
		}
	}

	delete(m.peers, peerID)
}

func (m *Manager) CreateRoom(roomID string, publisher *Peer) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.rooms[roomID]; exists {
		return fmt.Errorf("room %s already exists", roomID)
	}

	room := NewRoom(roomID, publisher, m.maxSubscribers)
	m.rooms[roomID] = room
	publisher.RoomID = roomID
	publisher.IsPublisher = true

	log.Printf("[room] created room %s by publisher %s", roomID, publisher.ID)
	return nil
}

func (m *Manager) JoinRoom(roomID string, subscriber *Peer) error {
	m.mu.RLock()
	room, exists := m.rooms[roomID]
	m.mu.RUnlock()

	if !exists {
		return fmt.Errorf("room %s not found", roomID)
	}

	if err := room.AddSubscriber(subscriber); err != nil {
		return err
	}

	subscriber.RoomID = roomID
	subscriber.IsPublisher = false

	log.Printf("[room] subscriber %s joined room %s (%d/%d)",
		subscriber.ID, roomID, room.SubscriberCount(), room.MaxSubscribers)

	// Notify publisher that a new subscriber joined
	if room.Publisher != nil {
		room.Publisher.Send(&model.SignalMessage{
			Type: model.MsgPeerJoined,
			From: subscriber.ID,
			Room: roomID,
		})
	}

	return nil
}

func (m *Manager) GetRoom(roomID string) *Room {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.rooms[roomID]
}

func (m *Manager) GetPeer(peerID string) *Peer {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.peers[peerID]
}

func (m *Manager) ForwardMessage(msg *model.SignalMessage) error {
	m.mu.RLock()
	target, ok := m.peers[msg.To]
	m.mu.RUnlock()

	if !ok {
		return fmt.Errorf("peer %s not found", msg.To)
	}

	return target.Send(msg)
}

// ReadMessage reads a length-prefixed JSON message from a connection.
func ReadMessage(conn net.Conn) (*model.SignalMessage, error) {
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return nil, err
	}
	msgLen := binary.BigEndian.Uint32(lenBuf)
	if msgLen == 0 || msgLen > 65536 {
		return nil, fmt.Errorf("invalid message length: %d", msgLen)
	}

	data := make([]byte, msgLen)
	if _, err := io.ReadFull(conn, data); err != nil {
		return nil, err
	}

	var msg model.SignalMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return nil, err
	}
	return &msg, nil
}
