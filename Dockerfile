FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    MMLLM_DEVICE=cpu \
    MMLLM_CONFIG=/app/configs/model.yaml

WORKDIR /app
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir --upgrade pip \
 && pip install --no-cache-dir -r /app/requirements.txt

COPY src /app/src
COPY configs /app/configs
COPY pyproject.toml /app/pyproject.toml
RUN pip install --no-cache-dir -e /app

EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/health').read()"

CMD ["uvicorn", "mmllm.serve:app", "--host", "0.0.0.0", "--port", "8000"]
