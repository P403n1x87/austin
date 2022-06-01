from time import sleep


class Foo:
    def run(self):
        sleep(0.5)


class Bar:
    def run(self):
        sleep(0.3)


Foo().run()
Bar().run()
