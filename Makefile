.PHONY: install test run docker

install:
	pip install -e '.[dev]'

test:
	pytest -q

run:
	uvicorn mmllm.serve:app --host 0.0.0.0 --port 8000

docker:
	docker build -t mmllm-decree .
