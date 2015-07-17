
function main()
{
	func1({
	    'obj1smalldate': new Date(),
	    'obj2bigdate': new Date(0),
	    'obj3null': null,
	    'obj4undef': undefined,
	    'obj5num': 5,
	    'obj6obj': {
	        'obj7arr': [ 1, 7, 7, 6 ]
	    },
	    'obj8str': 'it was the blurst of times!',
	    'obj8re': new RegExp('^hello, "\d+" worlds!$')
	});
}

function func1(obj)
{
	func2(obj);
}

function func2(obj, extra)
{
	for (;;)
		;
	console.log(obj);
}

main();
