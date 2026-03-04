package middleware

import (
	"context"
	"net/http"
	"strings"

	"github.com/libp2pchannel/signaling-server/auth"
)

type ctxKey string

const PeerIDKey ctxKey = "peer_id"

// Auth returns middleware that validates JWT tokens.
//
// /v1/events  -- token from ?token= query param
// /v1/admin/* -- token from Authorization: Bearer header (validated against adminSecret)
// /v1/signal  -- token is validated inside the handler (from JSON body), so this
//
//	middleware is a no-op for that path.
func Auth(jwtSecret, adminSecret string) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			path := r.URL.Path

			// Admin endpoints use a separate shared secret
			if strings.HasPrefix(path, "/v1/admin/") {
				hdr := r.Header.Get("Authorization")
				if !strings.HasPrefix(hdr, "Bearer ") {
					http.Error(w, `{"error":"missing Authorization header"}`, http.StatusUnauthorized)
					return
				}
				token := strings.TrimPrefix(hdr, "Bearer ")
				if token != adminSecret {
					http.Error(w, `{"error":"invalid admin token"}`, http.StatusForbidden)
					return
				}
				next.ServeHTTP(w, r)
				return
			}

			// SSE events endpoint uses query param
			if path == "/v1/events" {
				tokenStr := r.URL.Query().Get("token")
				if tokenStr == "" {
					http.Error(w, `{"error":"missing token"}`, http.StatusUnauthorized)
					return
				}
				peerID, err := auth.ValidateToken(tokenStr, jwtSecret)
				if err != nil {
					http.Error(w, `{"error":"invalid token"}`, http.StatusForbidden)
					return
				}
				ctx := context.WithValue(r.Context(), PeerIDKey, peerID)
				next.ServeHTTP(w, r.WithContext(ctx))
				return
			}

			// /v1/signal -- JWT validated inside handler from JSON body
			next.ServeHTTP(w, r)
		})
	}
}
