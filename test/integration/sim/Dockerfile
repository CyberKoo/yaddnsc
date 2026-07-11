FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /sim
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY server.py .

EXPOSE 53/udp 5353/udp 853/tcp 443/tcp 8080/tcp

ENTRYPOINT ["python", "-u", "server.py"]
