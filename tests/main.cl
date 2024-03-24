class Person {
    name: String;
    food: String;

    init(n: String, f: String): SELF_TYPE {
        {
            name <- n;
            food <- f;
            self;
        }
    };

    to_string(): String {
        name.concat(" likes to eat ").concat(food)
    };
};

class Main inherits IO {
    main(): Object {
        let
            void: List,
            guy: Person <- new Person.init("John", "pizza"),
            l: List <- new List.init("first", new List.init("second", void)),
            x: Int
        in
            {
                x <- in_int();
                l.append(x);
                l.append(true);
                l.append(new Object);
                l.append(guy);
                l.append("1".ord());
                while not isvoid l loop
                    {
                        out_string(l.value().to_string()).out_string("\n");
                        l <- l.next();
                    }
                pool;
            }
    };
};
