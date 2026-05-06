
  1. Buy a domain from a registrar

  - Examples: Cloudflare Registrar, Namecheap, Porkbun, GoDaddy
  - Pick something like yourname-webrtc.com

  2. Add DNS record

  - Create A record:
      - Host: @
      - Value: your public IPv4 (e.g. 14.136.97.137)
  - Optional subdomain:
      - Host: webrtc
      - Value: same IP
      - Then use webrtc.yourdomain.com

  3. Wait for propagation

  - Usually minutes, sometimes up to 24h
  - Check:

  dig +short yourdomain.com
  dig +short webrtc.yourdomain.com

  4. Configure your project

  - Set PUBLIC_FQDN in .env to that domain
  - Keep Caddy on host ports 80 and 443 for automatic TLS
  - Restart:

  cd /home/yeliu/Development/Esp32Demos/webrtc_public_gateway
  docker compose down
  docker compose up -d

  5. Verify cert issuance

  docker logs webrtc-edge --tail 200
