package turn

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"time"
)

/*
GenerateCredentials produces time-limited TURN credentials using the
coturn REST API mechanism (draft-uberti-behave-turn-rest-00).

  - username = timestamp:arbitrary_id
  - password = HMAC-SHA1(shared_secret, username)
*/
func GenerateCredentials(sharedSecret, peerID string, ttlSeconds uint32) (username, password string) {
	expiry := time.Now().Unix() + int64(ttlSeconds)
	username = fmt.Sprintf("%d:%s", expiry, peerID)

	mac := hmac.New(sha1.New, []byte(sharedSecret))
	mac.Write([]byte(username))
	password = base64.StdEncoding.EncodeToString(mac.Sum(nil))

	return username, password
}
