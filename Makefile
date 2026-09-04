.PHONY: all test clean
all:
	$(MAKE) -C engine
test:
	$(MAKE) -C engine test
clean:
	$(MAKE) -C engine clean
