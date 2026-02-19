IMAGE := baikal-arm/kernel
TAG := 1.0
SRCROOT := /home/ndz/gitlab
NAME := bookworm-arm64

all: Dockerfile
	docker image build --build-arg uid=1000 -t $(IMAGE):$(TAG) .
run:
	docker run -it -d -v $(SRCROOT):/home/user/workspace --name $(NAME) $(IMAGE):$(TAG) /bin/bash
	#docker exec -it $(NAME) /bin/bash
push:
	docker image tag $(IMAGE):$(TAG) gitlab.elpitech.ru:5050/$(IMAGE):$(TAG) 
	docker image push gitlab.elpitech.ru:5050/$(IMAGE):$(TAG) 
